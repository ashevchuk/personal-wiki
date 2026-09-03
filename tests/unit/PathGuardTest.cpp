// Traversal tests for wikicore::vault::PathGuard — written and run before
// any other code is allowed to touch the vault filesystem, per the plan
// (Milestone 1). If these don't all pass, nothing else in `vault/` may be
// trusted.

#include "vault/PathGuard.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using wikicore::vault::PathGuard;
using wikicore::vault::PathTraversalError;

namespace {

// RAII temp vault: a fresh, empty directory tree per test, removed on
// destruction regardless of how the test exits.
class TempVault {
 public:
  TempVault() {
    root_ = fs::temp_directory_path() /
            fs::path("wiki-pathguard-test-" +
                      std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    fs::remove_all(root_);
    fs::create_directories(root_);
  }
  ~TempVault() { fs::remove_all(root_); }

  TempVault(const TempVault&) = delete;
  TempVault& operator=(const TempVault&) = delete;

  const fs::path& root() const { return root_; }

  void makeFile(const fs::path& relative) const {
    const fs::path full = root_ / relative;
    fs::create_directories(full.parent_path());
    std::ofstream(full) << "content";
  }

  void makeDir(const fs::path& relative) const {
    fs::create_directories(root_ / relative);
  }

 private:
  fs::path root_;
};

}  // namespace

TEST_CASE("PathGuard accepts ordinary relative paths inside the vault",
          "[PathGuard]") {
  TempVault vault;
  vault.makeFile("notes/linux/systemd-timers.md");
  PathGuard guard(vault.root());

  const fs::path resolved = guard.resolve("notes/linux/systemd-timers.md");
  REQUIRE(resolved == fs::canonical(vault.root()) /
                           "notes" / "linux" / "systemd-timers.md");
}

TEST_CASE("PathGuard resolves '.' to the vault root", "[PathGuard]") {
  TempVault vault;
  PathGuard guard(vault.root());
  REQUIRE(guard.resolve(".") == fs::canonical(vault.root()));
}

TEST_CASE(
    "PathGuard accepts internal '..' that stays within the vault",
    "[PathGuard]") {
  TempVault vault;
  vault.makeFile("a/c.md");
  PathGuard guard(vault.root());

  // a/b/../c.md never leaves the vault even though it contains "..".
  const fs::path resolved = guard.resolve("a/b/../c.md");
  REQUIRE(resolved == fs::canonical(vault.root()) / "a" / "c.md");
}

TEST_CASE("PathGuard rejects a simple '..' escape", "[PathGuard]") {
  TempVault vault;
  PathGuard guard(vault.root());
  REQUIRE_THROWS_AS(guard.resolve("../etc/passwd"), PathTraversalError);
}

TEST_CASE("PathGuard rejects a deep '..' escape", "[PathGuard]") {
  TempVault vault;
  PathGuard guard(vault.root());
  REQUIRE_THROWS_AS(guard.resolve("../../../../etc/passwd"),
                     PathTraversalError);
}

TEST_CASE("PathGuard rejects an absolute path", "[PathGuard]") {
  TempVault vault;
  PathGuard guard(vault.root());
  REQUIRE_THROWS_AS(guard.resolve("/etc/passwd"), PathTraversalError);
}

TEST_CASE("PathGuard rejects an empty path", "[PathGuard]") {
  TempVault vault;
  PathGuard guard(vault.root());
  REQUIRE_THROWS_AS(guard.resolve(""), PathTraversalError);
}

TEST_CASE("PathGuard rejects an embedded NUL byte", "[PathGuard]") {
  TempVault vault;
  PathGuard guard(vault.root());
  const std::string malicious("notes/good.md\0/../../etc/passwd", 32);
  REQUIRE_THROWS_AS(guard.resolve(malicious), PathTraversalError);
}

TEST_CASE(
    "PathGuard does not itself decode percent-escapes — a literal "
    "'%2e%2e' segment is just a harmless filename, not '..'",
    "[PathGuard]") {
  TempVault vault;
  PathGuard guard(vault.root());
  // Never URL-decoded, so this resolves to a (non-existent) file literally
  // named "%2e%2e" inside the vault — it does not escape.
  const fs::path resolved = guard.resolve("%2e%2e/etc/passwd");
  const fs::path base = fs::canonical(vault.root());
  REQUIRE(resolved.lexically_relative(base).begin()->string() != "..");
}

TEST_CASE("PathGuard rejects a symlink planted inside the vault that "
          "points outside it",
          "[PathGuard]") {
  TempVault vault;
  TempVault outside;
  outside.makeFile("secret.txt");

  const fs::path linkPath = vault.root() / "escape_link";
  fs::create_directory_symlink(fs::canonical(outside.root()), linkPath);

  PathGuard guard(vault.root());
  REQUIRE_THROWS_AS(guard.resolve("escape_link/secret.txt"),
                     PathTraversalError);
}

TEST_CASE(
    "PathGuard accepts a symlink planted inside the vault that stays "
    "inside it",
    "[PathGuard]") {
  TempVault vault;
  vault.makeDir("real_target");
  vault.makeFile("real_target/doc.md");

  const fs::path linkPath = vault.root() / "link_inside";
  fs::create_directory_symlink(vault.root() / "real_target", linkPath);

  PathGuard guard(vault.root());
  const fs::path resolved = guard.resolve("link_inside/doc.md");
  REQUIRE(resolved ==
          fs::canonical(vault.root()) / "real_target" / "doc.md");
}

TEST_CASE(
    "PathGuard rejects a non-existent path whose lexical tail tries to "
    "climb back out via a symlinked existing prefix",
    "[PathGuard]") {
  TempVault vault;
  TempVault outside;

  const fs::path linkPath = vault.root() / "escape_link";
  fs::create_directory_symlink(fs::canonical(outside.root()), linkPath);

  PathGuard guard(vault.root());
  // "new-file.md" under the escaped symlink doesn't exist yet, but the
  // existing prefix (escape_link -> outside) already resolves outside the
  // vault, so this must still be rejected.
  REQUIRE_THROWS_AS(guard.resolve("escape_link/new-file.md"),
                     PathTraversalError);
}
