#include "server/CertWatcher.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace wikicore::server {

namespace {

constexpr auto kDebounce = std::chrono::milliseconds(300);
constexpr int kPollTimeoutMs = 100;
constexpr size_t kEventBufSize = 64 * (sizeof(struct inotify_event) + NAME_MAX + 1);

constexpr uint32_t kWatchMask = IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_CLOSE_WRITE;

}  // namespace

CertWatcher::CertWatcher(fs::path certFile, fs::path keyFile, ReloadCallback onValidReload)
    : certFile_(std::move(certFile)),
      keyFile_(std::move(keyFile)),
      watchDir_(certFile_.parent_path()),
      onValidReload_(std::move(onValidReload)) {}

CertWatcher::~CertWatcher() { stop(); }

bool CertWatcher::filesLookValid(const fs::path& certFile, const fs::path& keyFile) {
  std::error_code ec;
  if (!fs::is_regular_file(certFile, ec) || fs::file_size(certFile, ec) == 0) return false;
  if (!fs::is_regular_file(keyFile, ec) || fs::file_size(keyFile, ec) == 0) return false;

  BIO* certBio = BIO_new_file(certFile.c_str(), "r");
  if (!certBio) return false;
  X509* cert = PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr);
  BIO_free(certBio);
  if (!cert) return false;

  BIO* keyBio = BIO_new_file(keyFile.c_str(), "r");
  if (!keyBio) {
    X509_free(cert);
    return false;
  }
  EVP_PKEY* key = PEM_read_bio_PrivateKey(keyBio, nullptr, nullptr, nullptr);
  BIO_free(keyBio);
  if (!key) {
    X509_free(cert);
    return false;
  }

  // Each file parsing individually is not enough: trantor's own
  // newSSLContext() additionally calls SSL_CTX_check_private_key() after
  // loading both, and throws if the key doesn't match the cert's public
  // key — exactly the unrecoverable-on-reload failure mode this whole
  // class exists to prevent (see this file's header comment). A
  // certbot renewal updates the cert and key symlinks as two separate
  // filesystem operations, not one atomic transaction, so a transient
  // mismatched pair during that window is a real possibility, not a
  // hypothetical.
  const bool keyMatchesCert = X509_check_private_key(cert, key) == 1;

  X509_free(cert);
  EVP_PKEY_free(key);

  return keyMatchesCert;
}

void CertWatcher::start() {
  if (running_.exchange(true)) return;  // already running

  inotifyFd_ = inotify_init1(IN_NONBLOCK);
  if (inotifyFd_ < 0) {
    running_ = false;
    throw std::runtime_error(std::string("inotify_init1 failed: ") + std::strerror(errno));
  }
  wakeupFd_ = eventfd(0, EFD_NONBLOCK);
  if (wakeupFd_ < 0) {
    close(inotifyFd_);
    inotifyFd_ = -1;
    running_ = false;
    throw std::runtime_error(std::string("eventfd failed: ") + std::strerror(errno));
  }

  readyPromise_ = std::promise<void>();  // discard any prior (already-consumed) one
  std::future<void> ready = readyPromise_.get_future();

  thread_ = std::thread([this] { run(); });

  ready.wait();  // see the doc comment on start() for why this matters
}

void CertWatcher::stop() {
  if (!running_.exchange(false)) return;  // wasn't running
  if (wakeupFd_ >= 0) {
    const uint64_t one = 1;
    // Best-effort wakeup: if this write fails, the poll() timeout (100ms)
    // still bounds how long stop() waits below.
    if (write(wakeupFd_, &one, sizeof(one)) < 0) { /* see comment above */
    }
  }
  if (thread_.joinable()) thread_.join();
  if (inotifyFd_ >= 0) { close(inotifyFd_); inotifyFd_ = -1; }
  if (wakeupFd_ >= 0) { close(wakeupFd_); wakeupFd_ = -1; }
  pending_ = false;
}

void CertWatcher::run() {
  const int wd = inotify_add_watch(inotifyFd_, watchDir_.c_str(), kWatchMask);
  if (wd < 0) {
    std::cerr << "CertWatcher: failed to watch " << watchDir_
              << " — cert renewals will NOT be picked up automatically" << std::endl;
  }
  readyPromise_.set_value();

  std::vector<char> buf(kEventBufSize);

  while (running_.load()) {
    struct pollfd fds[2];
    fds[0].fd = inotifyFd_;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = wakeupFd_;
    fds[1].events = POLLIN;
    fds[1].revents = 0;

    const int rc = poll(fds, 2, kPollTimeoutMs);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;  // something's genuinely wrong with the fd; stop rather than spin
    }

    if (fds[1].revents & POLLIN) break;  // stop() signaled us

    if (fds[0].revents & POLLIN) {
      for (;;) {
        const ssize_t len = read(inotifyFd_, buf.data(), buf.size());
        if (len <= 0) break;  // EAGAIN (nothing more queued) or error -- stop draining

        size_t offset = 0;
        while (offset < static_cast<size_t>(len)) {
          const auto* ev = reinterpret_cast<const struct inotify_event*>(&buf[offset]);
          offset += sizeof(struct inotify_event) + ev->len;

          if (ev->mask & IN_Q_OVERFLOW) {
            // Lost events -- assume nothing about what changed, just
            // re-check on the next debounce pass below.
            pending_ = true;
            pendingSince_ = std::chrono::steady_clock::now();
            continue;
          }
          pending_ = true;
          pendingSince_ = std::chrono::steady_clock::now();
        }
      }
    }

    if (pending_ &&
        std::chrono::steady_clock::now() - pendingSince_ >= kDebounce) {
      pending_ = false;
      if (filesLookValid(certFile_, keyFile_)) {
        onValidReload_();
      } else {
        std::cerr << "CertWatcher: " << watchDir_
                  << " changed but cert/key failed PEM validation — skipping "
                     "reload, will retry on the next filesystem event"
                  << std::endl;
      }
    }
  }
}

}  // namespace wikicore::server
