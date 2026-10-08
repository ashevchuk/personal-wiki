#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <thread>

namespace wikicore::server {

// Watches the directory containing a TLS cert/key pair for changes (a
// certbot renewal replaces /etc/letsencrypt/live/<domain>/{fullchain,
// privkey}.pem via an atomic symlink swap inside that directory) and,
// once both files re-validate as parseable PEM, invokes onValidReload —
// typically drogon::app().reloadSSLFiles(). Mirrors index::VaultWatcher's
// inotify shape (ctor + start()/stop(), ready-promise, debounce); see
// that class's own comments for the underlying mechanics not repeated
// here. Unlike VaultWatcher this watches exactly one directory, not a
// tree, and debounces a single pending flag rather than a per-path map.
//
// The PEM pre-validation matters because reloadSSLFiles() re-reads the
// files asynchronously (on Drogon's own event-loop thread, via
// queueInLoop) — an invalid file at THAT point throws past any catch
// this class could offer, tearing down the event loop's thread with it
// (trantor's OpenSSLProvider::newSSLContext()). Never call the reload
// callback without validating first; on failure, skip and wait for the
// next filesystem event rather than retrying immediately.
class CertWatcher {
 public:
  using ReloadCallback = std::function<void()>;

  CertWatcher(std::filesystem::path certFile, std::filesystem::path keyFile,
              ReloadCallback onValidReload);
  ~CertWatcher();

  CertWatcher(const CertWatcher&) = delete;
  CertWatcher& operator=(const CertWatcher&) = delete;

  // Same blocking-until-registered contract as VaultWatcher::start(): a
  // change made the instant after start() returns is guaranteed to be
  // seen. No-op if already running.
  void start();

  // Stops the background thread and joins it. Safe to call from the
  // destructor's implicit path (called there too) or explicitly earlier.
  void stop();

  // True iff both files exist, are non-empty, successfully PEM-parse as
  // an X.509 certificate / private key respectively, AND the key
  // actually matches the certificate's public key (direct OpenSSL
  // calls, no Drogon/trantor involved). The key-match check matters as
  // much as the parsing: trantor's own newSSLContext() calls
  // SSL_CTX_check_private_key() after loading both and throws on a
  // mismatch, which is exactly the crash this class exists to prevent —
  // see the .cpp. Exposed so main.cpp can run the same check once at
  // startup, before ever calling addListener with useSSL=true.
  static bool filesLookValid(const std::filesystem::path& certFile,
                              const std::filesystem::path& keyFile);

 private:
  void run();

  std::filesystem::path certFile_;
  std::filesystem::path keyFile_;
  std::filesystem::path watchDir_;  // certFile_'s parent
  ReloadCallback onValidReload_;

  int inotifyFd_ = -1;
  int wakeupFd_ = -1;  // eventfd; writing to it interrupts poll() for stop()

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::promise<void> readyPromise_;  // fresh instance each start()

  // Debounce state, owned entirely by the watcher thread (no locking
  // needed — nothing else touches it). Only one thing to debounce here
  // ("something in watchDir_ changed"), unlike VaultWatcher's per-path map.
  bool pending_ = false;
  std::chrono::steady_clock::time_point pendingSince_{};
};

}  // namespace wikicore::server
