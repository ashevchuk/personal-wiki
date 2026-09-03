#include "vault/AttachmentService.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace fs = std::filesystem;
using namespace wikicore;

namespace {

class TempVaultDir {
 public:
  TempVaultDir()
      : root_(fs::temp_directory_path() /
              fs::path("wiki-attach-test-" +
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

}  // namespace

TEST_CASE("AttachmentService stores an allowed file under <stem>.assets/",
          "[AttachmentService]") {
  TempVaultDir dir;
  vault::VaultRepository repo(dir.root());
  vault::AttachmentService svc(repo);

  const auto info = svc.store("notes/foo.md", "diagram.png", "fake png bytes");
  REQUIRE(info.relativePath == "notes/foo.assets/diagram.png");
  REQUIRE(info.mimeType == "image/png");
  REQUIRE(info.size == 14);
  REQUIRE(fs::exists(dir.root() / "notes/foo.assets/diagram.png"));
}

TEST_CASE("AttachmentService rejects a disallowed extension", "[AttachmentService]") {
  TempVaultDir dir;
  vault::VaultRepository repo(dir.root());
  vault::AttachmentService svc(repo);

  REQUIRE_THROWS_AS(svc.store("notes/foo.md", "payload.exe", "x"),
                     vault::AttachmentRejectedError);
}

TEST_CASE("AttachmentService rejects an oversized upload", "[AttachmentService]") {
  TempVaultDir dir;
  vault::VaultRepository repo(dir.root());
  vault::AttachmentService svc(repo);

  const std::string tooBig(26LL * 1024 * 1024, 'x');
  REQUIRE_THROWS_AS(svc.store("notes/foo.md", "big.png", tooBig),
                     vault::AttachmentRejectedError);
}

TEST_CASE("AttachmentService sanitizes a filename with path separators / "
          "traversal segments down to a harmless basename",
          "[AttachmentService]") {
  TempVaultDir dir;
  vault::VaultRepository repo(dir.root());
  vault::AttachmentService svc(repo);

  const auto info = svc.store("notes/foo.md", "../../etc/evil.png", "x");
  // Only the basename survives fs::path(...).filename(); nothing here can
  // land outside notes/foo.assets/.
  REQUIRE(info.relativePath.rfind("notes/foo.assets/", 0) == 0);
  REQUIRE(info.relativePath.find("..") == std::string::npos);
}

TEST_CASE("AttachmentService de-dupes a filename collision instead of "
          "overwriting",
          "[AttachmentService]") {
  TempVaultDir dir;
  vault::VaultRepository repo(dir.root());
  vault::AttachmentService svc(repo);

  const auto first = svc.store("notes/foo.md", "img.png", "first");
  const auto second = svc.store("notes/foo.md", "img.png", "second");
  REQUIRE(first.relativePath != second.relativePath);

  vault::VaultRepository verify(dir.root());
  REQUIRE(verify.readRaw(first.relativePath) == "first");
  REQUIRE(verify.readRaw(second.relativePath) == "second");
}
