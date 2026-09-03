#include "index/VaultWatcher.h"

#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace wikicore::index {

namespace {

constexpr auto kDebounce = std::chrono::milliseconds(300);
constexpr int kPollTimeoutMs = 100;
// One directory's worth of events, generously sized (name + header) —
// re-read in a loop if a single read() doesn't drain everything pending.
constexpr size_t kEventBufSize = 64 * (sizeof(struct inotify_event) + NAME_MAX + 1);

bool isDotEntry(const fs::path& p) {
  const std::string name = p.filename().string();
  return !name.empty() && name[0] == '.';
}

constexpr uint32_t kWatchMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM |
                                IN_MOVED_TO | IN_CLOSE_WRITE;

}  // namespace

VaultWatcher::VaultWatcher(fs::path vaultRoot, ChangeCallback onChange,
                            OverflowCallback onOverflow)
    : root_(fs::canonical(std::move(vaultRoot))),
      onChange_(std::move(onChange)),
      onOverflow_(std::move(onOverflow)) {}

VaultWatcher::~VaultWatcher() { stop(); }

void VaultWatcher::start() {
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

void VaultWatcher::stop() {
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
  watchDirByDescriptor_.clear();
  pending_.clear();
}

void VaultWatcher::addWatchRecursive(const fs::path& dir, bool reportExisting) {
  const int wd = inotify_add_watch(inotifyFd_, dir.c_str(), kWatchMask);
  if (wd < 0) {
    // A directory can legitimately vanish between being listed by the
    // caller and us getting here (race with an external delete) --
    // that's not a program error, just skip it.
    return;
  }
  watchDirByDescriptor_[wd] = dir;

  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (ec) break;
    if (isDotEntry(entry.path())) continue;

    if (entry.is_directory()) {
      addWatchRecursive(entry.path(), reportExisting);
    } else if (reportExisting && entry.path().extension() == ".md") {
      markPending(fs::relative(entry.path(), root_).generic_string());
    }
  }
}

void VaultWatcher::markPending(const std::string& relativePath) {
  pending_[relativePath] = std::chrono::steady_clock::now();
}

void VaultWatcher::run() {
  addWatchRecursive(root_, /*reportExisting=*/false);
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
            if (onOverflow_) onOverflow_();
            continue;
          }
          if (ev->len == 0) continue;  // no name -- not a directory-entry event we care about

          const auto dirIt = watchDirByDescriptor_.find(ev->wd);
          if (dirIt == watchDirByDescriptor_.end()) continue;  // stale/removed watch
          const fs::path entryPath = dirIt->second / ev->name;

          if (ev->mask & IN_ISDIR) {
            if (ev->mask & (IN_CREATE | IN_MOVED_TO)) {
              if (!isDotEntry(entryPath)) addWatchRecursive(entryPath, /*reportExisting=*/true);
            }
            // A watched directory being removed needs no action here: the
            // kernel auto-deletes its watch, and any files that were
            // inside it already generated their own IN_DELETE events
            // (rmdir requires an empty directory).
            continue;
          }

          if (entryPath.extension() == ".md" && !isDotEntry(entryPath)) {
            markPending(fs::relative(entryPath, root_).generic_string());
          }
        }
      }
    }

    const auto now = std::chrono::steady_clock::now();
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (now - it->second >= kDebounce) {
        onChange_(it->first);
        it = pending_.erase(it);
      } else {
        ++it;
      }
    }
  }
}

}  // namespace wikicore::index
