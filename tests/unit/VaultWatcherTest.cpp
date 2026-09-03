#include "index/VaultWatcher.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using wikicore::index::VaultWatcher;

namespace {

class TempVaultDir {
 public:
  TempVaultDir()
      : root_(fs::temp_directory_path() /
              fs::path("wiki-watcher-test-" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this)))) {
    fs::remove_all(root_);
    fs::create_directories(root_);
  }
  ~TempVaultDir() { fs::remove_all(root_); }
  TempVaultDir(const TempVaultDir&) = delete;
  TempVaultDir& operator=(const TempVaultDir&) = delete;
  const fs::path& root() const { return root_; }

 private:
  fs::path root_;
};

// Thread-safe log of callback invocations, with a wait-for-condition
// helper — avoids flaky fixed sleeps by polling with a generous overall
// timeout instead.
class CallbackLog {
 public:
  void record(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(path);
    cv_.notify_all();
  }

  // Waits (up to `timeoutMs`) until at least `count` events for `path`
  // have been recorded. Returns the final count.
  size_t waitForCount(const std::string& path, size_t count, int timeoutMs = 3000) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
      return countLocked(path) >= count;
    });
    return countLocked(path);
  }

  size_t count(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    return countLocked(path);
  }

 private:
  size_t countLocked(const std::string& path) const {
    return static_cast<size_t>(std::count(events_.begin(), events_.end(), path));
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::string> events_;
};

}  // namespace

TEST_CASE("VaultWatcher reports a newly-created .md file", "[VaultWatcher]") {
  TempVaultDir dir;
  CallbackLog log;
  VaultWatcher watcher(
      dir.root(), [&](const std::string& p) { log.record(p); }, [] {});
  watcher.start();

  std::ofstream(dir.root() / "new.md") << "hello";

  REQUIRE(log.waitForCount("new.md", 1) >= 1);
  watcher.stop();
}

TEST_CASE("VaultWatcher coalesces rapid successive writes to one callback",
          "[VaultWatcher]") {
  TempVaultDir dir;
  CallbackLog log;
  VaultWatcher watcher(
      dir.root(), [&](const std::string& p) { log.record(p); }, [] {});
  watcher.start();

  // Several writes within the ~300ms debounce window should collapse.
  for (int i = 0; i < 5; ++i) {
    std::ofstream(dir.root() / "burst.md") << "v" << i;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  // Wait long enough for the debounce to fire, then confirm it only fired
  // once despite 5 writes.
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  REQUIRE(log.count("burst.md") == 1);
  watcher.stop();
}

TEST_CASE("VaultWatcher reports a file created inside a brand-new subdirectory",
          "[VaultWatcher]") {
  TempVaultDir dir;
  CallbackLog log;
  VaultWatcher watcher(
      dir.root(), [&](const std::string& p) { log.record(p); }, [] {});
  watcher.start();

  fs::create_directories(dir.root() / "notes");
  std::ofstream(dir.root() / "notes" / "sub.md") << "hi";

  REQUIRE(log.waitForCount("notes/sub.md", 1) >= 1);
  watcher.stop();
}

TEST_CASE("VaultWatcher ignores dotdirs like .git/.trash", "[VaultWatcher]") {
  TempVaultDir dir;
  CallbackLog log;
  VaultWatcher watcher(
      dir.root(), [&](const std::string& p) { log.record(p); }, [] {});
  watcher.start();

  fs::create_directories(dir.root() / ".git");
  std::ofstream(dir.root() / ".git" / "ignored.md") << "should not be seen";
  // A real (non-dot) file too, so we have a positive signal that the
  // watcher is alive and would have reported the dotdir file if it were
  // going to.
  std::ofstream(dir.root() / "real.md") << "seen";

  REQUIRE(log.waitForCount("real.md", 1) >= 1);
  REQUIRE(log.count(".git/ignored.md") == 0);
  watcher.stop();
}

TEST_CASE("VaultWatcher reports a deleted file (caller checks existence "
          "to decide upsert vs remove)",
          "[VaultWatcher]") {
  TempVaultDir dir;
  const fs::path filePath = dir.root() / "gone.md";
  std::ofstream(filePath) << "will be deleted";

  CallbackLog log;
  VaultWatcher watcher(
      dir.root(), [&](const std::string& p) { log.record(p); }, [] {});
  watcher.start();
  // Let the initial watch setup settle before deleting, so this event is
  // unambiguously the delete, not a startup artifact.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  fs::remove(filePath);

  REQUIRE(log.waitForCount("gone.md", 1) >= 1);
  REQUIRE_FALSE(fs::exists(filePath));
  watcher.stop();
}

TEST_CASE("VaultWatcher stop() is idempotent and start()/stop() can be "
          "called safely without a leaked thread",
          "[VaultWatcher]") {
  TempVaultDir dir;
  VaultWatcher watcher(
      dir.root(), [](const std::string&) {}, [] {});
  watcher.start();
  watcher.stop();
  watcher.stop();  // must not hang or throw
  SUCCEED();
}
