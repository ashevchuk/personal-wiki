#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <unordered_map>

namespace wikicore::index {

// Watches the vault directory tree for changes made outside the app
// (external editor, `git pull`, a sync tool, ...) and reports them so the
// SQLite index can be kept incrementally current, without waiting for the
// next full rescan. Best-effort, not a correctness guarantee: `wiki-server
// --reindex` / the startup rescan remain the authoritative fallback — see
// docs/architecture.md.
//
// inotify is not recursive: watches are added directory-by-directory, and
// a newly-created subdirectory gets a new watch (plus a scan of whatever
// already exists inside it, in case files landed there in the race
// between its creation and the watch being registered — e.g. an entire
// folder copied/moved in at once). `.git`/`.trash`/any dotdir is skipped,
// matching IndexBuilder's rescan.
//
// Events are debounced per relative path (~300ms, coalescing rapid
// event bursts — an atomic rename-based save generates more than one
// inotify event for the same logical change) and delivered as a single
// callback per path once things settle; the callback decides
// upsert-vs-remove itself by checking whether the file currently exists,
// which is simpler and more robust than trying to interpret raw event
// flags (CREATE/DELETE/MOVED_FROM/MOVED_TO) directly.
class VaultWatcher {
 public:
  using ChangeCallback = std::function<void(const std::string& relativePath)>;
  // Called (not debounced) if the kernel's inotify event queue overflowed
  // and events may have been lost — the only correct recovery is "assume
  // nothing about what changed, rescan everything."
  using OverflowCallback = std::function<void()>;

  VaultWatcher(std::filesystem::path vaultRoot, ChangeCallback onChange,
               OverflowCallback onOverflow);
  ~VaultWatcher();

  VaultWatcher(const VaultWatcher&) = delete;
  VaultWatcher& operator=(const VaultWatcher&) = delete;

  // Starts the background thread and BLOCKS until the initial recursive
  // watch registration completes — a change made the instant after
  // start() returns is guaranteed to be seen. (Without this guarantee, a
  // file changed in the gap between the thread merely existing and it
  // actually having called inotify_add_watch() on the relevant directory
  // is invisible forever, since the kernel never queues an event for a
  // watch that wasn't registered yet — a real race, not a hypothetical
  // one; caught by VaultWatcherTest.) No-op if already running.
  void start();

  // Stops the background thread and joins it. Safe to call from the
  // destructor's implicit path (called there too) or explicitly earlier.
  void stop();

 private:
  void run();
  // Adds a watch for `dir` and recurses into subdirectories (skipping
  // dotdirs). If `reportExisting` is true, every .md file found is also
  // treated as a pending change — used when a brand-new directory shows
  // up while already running, never for the initial startup walk (which
  // only needs to establish watches; the caller's own startup rescan
  // already covers pre-existing content).
  void addWatchRecursive(const std::filesystem::path& dir, bool reportExisting);
  void markPending(const std::string& relativePath);

  std::filesystem::path root_;
  ChangeCallback onChange_;
  OverflowCallback onOverflow_;

  int inotifyFd_ = -1;
  int wakeupFd_ = -1;  // eventfd; writing to it interrupts poll() for stop()
  std::unordered_map<int, std::filesystem::path> watchDirByDescriptor_;

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::promise<void> readyPromise_;  // fresh instance each start()

  // Debounce state, owned entirely by the watcher thread (no locking
  // needed — nothing else touches it).
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> pending_;
};

}  // namespace wikicore::index
