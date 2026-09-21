#include "vault/AttachmentService.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

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

TEST_CASE("AttachmentService accepts any extension, with a generic MIME "
          "type fallback for anything it doesn't recognize",
          "[AttachmentService]") {
  TempVaultDir dir;
  vault::VaultRepository repo(dir.root());
  vault::AttachmentService svc(repo);

  // No extension allowlist/blocklist anymore — see the class comment.
  // Even a genuinely unusual extension is accepted; only the MIME type
  // guess falls back to a generic value.
  const auto info = svc.store("notes/foo.md", "payload.exe", "x");
  REQUIRE(info.relativePath == "notes/foo.assets/payload.exe");
  REQUIRE(info.mimeType == "application/octet-stream");

  const auto ini = svc.store("notes/foo.md", "config.ini", "[section]\nkey=value\n");
  REQUIRE(ini.relativePath == "notes/foo.assets/config.ini");
  REQUIRE(ini.mimeType == "text/plain");
}

TEST_CASE("AttachmentService::isSafeToRenderInline is a small curated "
          "allowlist, not the reverse of a blocklist",
          "[AttachmentService]") {
  TempVaultDir dir;
  vault::VaultRepository repo(dir.root());
  vault::AttachmentService svc(repo);  // default mime/inline-safe tables

  REQUIRE(svc.isSafeToRenderInline("png"));
  REQUIRE(svc.isSafeToRenderInline("pdf"));
  REQUIRE(svc.isSafeToRenderInline("txt"));
  // These execute same-origin JS if a browser navigates straight to
  // GET /assets/{path...} — must stay forced-download regardless of any
  // future extension being added elsewhere.
  REQUIRE_FALSE(svc.isSafeToRenderInline("html"));
  REQUIRE_FALSE(svc.isSafeToRenderInline("htm"));
  REQUIRE_FALSE(svc.isSafeToRenderInline("svg"));
  REQUIRE_FALSE(svc.isSafeToRenderInline("xml"));
  REQUIRE_FALSE(svc.isSafeToRenderInline("exe"));
  REQUIRE_FALSE(svc.isSafeToRenderInline("ini"));
}

TEST_CASE("AttachmentService honors config-provided mime/inline-safe "
          "tables instead of the built-in defaults when given one",
          "[AttachmentService]") {
  TempVaultDir dir;
  vault::VaultRepository repo(dir.root());
  vault::AttachmentService svc(repo, {{"ini", "application/x-my-ini"}}, {"ini"});

  const auto info = svc.store("notes/foo.md", "config.ini", "[x]\n");
  REQUIRE(info.mimeType == "application/x-my-ini");
  REQUIRE(svc.isSafeToRenderInline("ini"));
  // Not in the custom table at all -> falls back to the generic type,
  // same as an unrecognized extension always does.
  REQUIRE(svc.mimeTypeForExtension("png") == "application/octet-stream");
}

TEST_CASE("AttachmentService::storeFromPath copies from disk without a size cap",
          "[AttachmentService]") {
  TempVaultDir dir;
  vault::VaultRepository repo(dir.root());
  vault::AttachmentService svc(repo);

  const fs::path src = dir.root() / "outside.bin";
  {
    std::ofstream out(src, std::ios::binary);
    out << std::string(4096, 'Q');
  }

  const auto info = svc.storeFromPath("notes/foo.md", "paper.pdf", src);
  REQUIRE(info.relativePath == "notes/foo.assets/paper.pdf");
  REQUIRE(info.mimeType == "application/pdf");
  REQUIRE(info.size == 4096);
  REQUIRE(repo.readRaw("notes/foo.assets/paper.pdf") == std::string(4096, 'Q'));
}

TEST_CASE("AttachmentService::storeFromPath rejects a missing or non-file source",
          "[AttachmentService]") {
  TempVaultDir dir;
  vault::VaultRepository repo(dir.root());
  vault::AttachmentService svc(repo);

  REQUIRE_THROWS_AS(svc.storeFromPath("notes/foo.md", "x.bin", dir.root() / "nope.bin"),
                     vault::AttachmentRejectedError);
  REQUIRE_THROWS_AS(svc.storeFromPath("notes/foo.md", "x.bin", dir.root()),
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

TEST_CASE("AttachmentService::listForDocument lists stored files, empty if none",
          "[AttachmentService]") {
  TempVaultDir dir;
  vault::VaultRepository repo(dir.root());
  vault::AttachmentService svc(repo);

  REQUIRE(svc.listForDocument("notes/foo.md").empty());

  svc.store("notes/foo.md", "b.txt", "bb");
  svc.store("notes/foo.md", "a.txt", "aa");
  const auto listed = svc.listForDocument("notes/foo.md");
  REQUIRE(listed.size() == 2);
  REQUIRE(listed[0].relativePath == "notes/foo.assets/a.txt");
  REQUIRE(listed[1].relativePath == "notes/foo.assets/b.txt");
  REQUIRE(listed[0].size == 2);
  REQUIRE(listed[0].mimeType == "text/plain");
}

TEST_CASE("AttachmentService::remove deletes the file and the empty assets dir",
          "[AttachmentService]") {
  TempVaultDir dir;
  vault::VaultRepository repo(dir.root());
  vault::AttachmentService svc(repo);

  const auto info = svc.store("notes/foo.md", "x.bin", "xyz");
  REQUIRE(svc.remove(info.relativePath));
  REQUIRE_FALSE(fs::exists(dir.root() / "notes/foo.assets/x.bin"));
  REQUIRE_FALSE(fs::exists(dir.root() / "notes/foo.assets"));
  REQUIRE_FALSE(svc.remove(info.relativePath));
}

TEST_CASE("AttachmentService::remove refuses a path that is not under .assets/",
          "[AttachmentService]") {
  TempVaultDir dir;
  vault::VaultRepository repo(dir.root());
  vault::AttachmentService svc(repo);
  svc.store("notes/foo.md", "x.bin", "xyz");

  REQUIRE_THROWS_AS(svc.remove("notes/foo.md"), vault::AttachmentRejectedError);
  REQUIRE_THROWS_AS(svc.remove("notes/foo.assets"), vault::AttachmentRejectedError);
  REQUIRE(fs::exists(dir.root() / "notes/foo.assets/x.bin"));
}
