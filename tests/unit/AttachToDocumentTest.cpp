#include "index/Database.h"
#include "index/IndexUpdater.h"
#include "index/SnapshotStore.h"
#include "vault/AttachToDocument.h"
#include "vault/AttachmentService.h"
#include "vault/DocumentService.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace wikicore;

namespace {

class TempEnv {
 public:
  TempEnv()
      : root_(fs::temp_directory_path() /
              fs::path("wiki-attach-doc-test-" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this)))) {
    fs::remove_all(root_);
    fs::create_directories(root_ / "vault");
  }
  ~TempEnv() { fs::remove_all(root_); }
  TempEnv(const TempEnv&) = delete;
  TempEnv& operator=(const TempEnv&) = delete;
  fs::path vaultRoot() const { return root_ / "vault"; }
  fs::path dbPath() const { return root_ / "index.db"; }

 private:
  fs::path root_;
};

}  // namespace

TEST_CASE("attachFileAndLink stores the file and appends an image markdown link",
          "[AttachToDocument]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService docs(repo, indexUpdater, snapshots);
  vault::AttachmentService attachments(repo);

  vault::DocumentInput input;
  input.title = "Move Semantics";
  input.body = "Some notes.\n";
  input.visibility = "private";
  input.type = "note";
  docs.create("notes/cpp/move-semantics.md", input);

  const auto attached = vault::attachFileAndLink(
      docs, attachments, "notes/cpp/move-semantics.md", "diagram.png", "fake png");

  REQUIRE(attached.info.relativePath == "notes/cpp/move-semantics.assets/diagram.png");
  REQUIRE(attached.info.mimeType == "image/png");
  REQUIRE(attached.markdownLink ==
          "![diagram.png](assets/notes/cpp/move-semantics.assets/diagram.png)");
  REQUIRE(fs::exists(env.vaultRoot() / "notes/cpp/move-semantics.assets/diagram.png"));

  const auto after = docs.get("notes/cpp/move-semantics.md");
  REQUIRE(after.body ==
          "Some notes.\n\n"
          "![diagram.png](assets/notes/cpp/move-semantics.assets/diagram.png)\n");
}

TEST_CASE("attachFileAndLink uses a regular markdown link for non-images",
          "[AttachToDocument]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService docs(repo, indexUpdater, snapshots);
  vault::AttachmentService attachments(repo);

  vault::DocumentInput input;
  input.title = "Notes";
  input.body = "";
  docs.create("notes/a.md", input);

  const auto attached =
      vault::attachFileAndLink(docs, attachments, "notes/a.md", "spec.pdf", "pdf-bytes");
  REQUIRE(attached.markdownLink == "[spec.pdf](assets/notes/a.assets/spec.pdf)");
  REQUIRE(docs.get("notes/a.md").body == "[spec.pdf](assets/notes/a.assets/spec.pdf)\n");
}

TEST_CASE("attachFileAndLink refuses a missing owning document before writing",
          "[AttachToDocument]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService docs(repo, indexUpdater, snapshots);
  vault::AttachmentService attachments(repo);

  REQUIRE_THROWS_AS(
      vault::attachFileAndLink(docs, attachments, "notes/missing.md", "x.png", "x"),
      vault::DocumentNotFoundError);
  REQUIRE_FALSE(fs::exists(env.vaultRoot() / "notes/missing.assets"));
}

TEST_CASE("svg is linked, not embedded — it is not inline-safe",
          "[AttachToDocument]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService docs(repo, indexUpdater, snapshots);
  vault::AttachmentService attachments(repo);

  vault::DocumentInput input;
  input.title = "Notes";
  input.body = "hi";
  docs.create("notes/a.md", input);

  const auto attached =
      vault::attachFileAndLink(docs, attachments, "notes/a.md", "icon.svg", "<svg/>");
  REQUIRE(attached.info.mimeType == "image/svg+xml");
  REQUIRE(attached.markdownLink == "[icon.svg](assets/notes/a.assets/icon.svg)");
}

TEST_CASE("attachFileAndLinkFromPath copies a host file and links it",
          "[AttachToDocument]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService docs(repo, indexUpdater, snapshots);
  vault::AttachmentService attachments(repo);

  vault::DocumentInput input;
  input.title = "Notes";
  input.body = "see attached";
  docs.create("notes/a.md", input);

  const fs::path src = env.vaultRoot() / "incoming.pdf";
  {
    std::ofstream out(src, std::ios::binary);
    out << "%PDF-fake";
  }

  const auto attached =
      vault::attachFileAndLinkFromPath(docs, attachments, "notes/a.md", "paper.pdf", src);
  REQUIRE(attached.info.size == 9);
  REQUIRE(attached.markdownLink == "[paper.pdf](assets/notes/a.assets/paper.pdf)");
  REQUIRE(docs.get("notes/a.md").body.find(attached.markdownLink) != std::string::npos);
  REQUIRE(repo.readRaw("notes/a.assets/paper.pdf") == "%PDF-fake");
}
