#include "index/Database.h"
#include "index/IndexUpdater.h"
#include "index/NavQueries.h"
#include "index/SnapshotStore.h"
#include "vault/DocumentService.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace wikicore;

namespace {

// RAII temp vault + index db per test, mirroring PathGuardTest's TempVault.
class TempEnv {
 public:
  TempEnv()
      : root_(fs::temp_directory_path() /
              fs::path("wiki-docsvc-test-" +
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

TEST_CASE("DocumentService create/get round-trips front matter and body",
          "[DocumentService]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService svc(repo, indexUpdater, snapshots);

  vault::DocumentInput input;
  input.title = "Systemd timers";
  input.tags = {"linux", "systemd"};
  input.visibility = "public";
  input.type = "note";
  input.body = "Some *markdown* body.\n";

  const auto created = svc.create("notes/systemd-timers.md", input);
  REQUIRE(!created.frontMatter.id.empty());
  REQUIRE(created.frontMatter.title == "Systemd timers");
  REQUIRE(created.frontMatter.visibility == "public");
  REQUIRE(!created.frontMatter.created.empty());
  REQUIRE(created.frontMatter.created == created.frontMatter.updated);

  const auto fetched = svc.get("notes/systemd-timers.md");
  REQUIRE(fetched.frontMatter.id == created.frontMatter.id);
  REQUIRE(fetched.frontMatter.tags == std::vector<std::string>{"linux", "systemd"});
  REQUIRE(fetched.body == "Some *markdown* body.\n");
}

TEST_CASE("DocumentService create rejects a path that already exists",
          "[DocumentService]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService svc(repo, indexUpdater, snapshots);

  vault::DocumentInput input;
  input.title = "One";
  input.body = "x";
  svc.create("a.md", input);
  REQUIRE_THROWS_AS(svc.create("a.md", input), vault::DocumentAlreadyExistsError);
}

TEST_CASE("DocumentService update preserves id/created, bumps updated, "
          "rejects an unknown path",
          "[DocumentService]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService svc(repo, indexUpdater, snapshots);

  vault::DocumentInput input;
  input.title = "Original";
  input.visibility = "private";
  input.body = "v1";
  const auto created = svc.create("a.md", input);

  vault::DocumentInput updateInput;
  updateInput.title = "Changed";
  updateInput.visibility = "public";
  updateInput.body = "v2";
  const auto updated = svc.update("a.md", updateInput);

  REQUIRE(updated.frontMatter.id == created.frontMatter.id);
  REQUIRE(updated.frontMatter.created == created.frontMatter.created);
  REQUIRE(updated.frontMatter.title == "Changed");
  REQUIRE(updated.frontMatter.visibility == "public");
  REQUIRE(updated.body == "v2");

  vault::DocumentInput irrelevant;
  REQUIRE_THROWS_AS(svc.update("does-not-exist.md", irrelevant),
                     vault::DocumentNotFoundError);
}

TEST_CASE("DocumentService::update snapshots the PRE-edit state, "
          "create() snapshots nothing",
          "[DocumentService]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService svc(repo, indexUpdater, snapshots);

  vault::DocumentInput v1;
  v1.title = "V1";
  v1.body = "version one";
  svc.create("a.md", v1);

  const auto rowId = indexUpdater.rowIdForPath("a.md");
  REQUIRE(rowId.has_value());
  // create() must not have snapshotted anything -- there's no "before"
  // state for a brand-new document.
  REQUIRE(snapshots.list(*rowId).empty());

  vault::DocumentInput v2;
  v2.title = "V2";
  v2.body = "version two";
  svc.update("a.md", v2);

  const auto afterFirstUpdate = snapshots.list(*rowId);
  REQUIRE(afterFirstUpdate.size() == 1);
  // The snapshot holds the PRE-edit (v1) content, not v2 -- serialized
  // front matter + body, so checking for the body text is enough
  // without depending on exact YAML formatting.
  const auto content = snapshots.getContent(*rowId, afterFirstUpdate[0].id);
  REQUIRE(content.has_value());
  REQUIRE(content->find("version one") != std::string::npos);
  REQUIRE(content->find("version two") == std::string::npos);

  vault::DocumentInput v3;
  v3.title = "V3";
  v3.body = "version three";
  svc.update("a.md", v3);

  // Two edits after the initial create -> two snapshots, newest first.
  const auto afterSecondUpdate = snapshots.list(*rowId);
  REQUIRE(afterSecondUpdate.size() == 2);
  const auto newest = snapshots.getContent(*rowId, afterSecondUpdate[0].id);
  REQUIRE(newest->find("version two") != std::string::npos);
}

TEST_CASE("DocumentService update falls back to visibility=private for "
          "anything other than exactly 'public'",
          "[DocumentService]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService svc(repo, indexUpdater, snapshots);

  vault::DocumentInput input;
  input.visibility = "PUBLIC";  // wrong case — must not slip through
  input.body = "x";
  const auto created = svc.create("a.md", input);
  REQUIRE(created.frontMatter.visibility == "private");
}

TEST_CASE("DocumentService softDelete moves the file (and assets folder) to "
          ".trash/ and removes it from the index",
          "[DocumentService]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService svc(repo, indexUpdater, snapshots);

  vault::DocumentInput input;
  input.body = "x";
  svc.create("notes/a.md", input);
  fs::create_directories(env.vaultRoot() / "notes/a.assets");
  std::ofstream(env.vaultRoot() / "notes/a.assets/img.png") << "fake";

  svc.softDelete("notes/a.md");

  REQUIRE_FALSE(fs::exists(env.vaultRoot() / "notes/a.md"));
  REQUIRE(fs::exists(env.vaultRoot() / ".trash/notes/a.md"));
  REQUIRE(fs::exists(env.vaultRoot() / ".trash/notes/a.assets/img.png"));
  REQUIRE_THROWS_AS(svc.softDelete("notes/a.md"), vault::DocumentNotFoundError);
}

TEST_CASE("DocumentService create appends .md when the caller omitted it",
          "[DocumentService]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService svc(repo, indexUpdater, snapshots);

  vault::DocumentInput input;
  input.title = "No suffix";
  input.body = "x";
  const auto created = svc.create("notes/smart-pointers", input);
  REQUIRE(created.path == "notes/smart-pointers.md");
  REQUIRE(fs::exists(env.vaultRoot() / "notes/smart-pointers.md"));
  REQUIRE_FALSE(fs::exists(env.vaultRoot() / "notes/smart-pointers"));

  const auto already = svc.create("notes/other.md", input);
  REQUIRE(already.path == "notes/other.md");

  const auto mixedCase = svc.create("notes/Legacy.MD", input);
  REQUIRE(mixedCase.path == "notes/Legacy.md");
  REQUIRE(fs::exists(env.vaultRoot() / "notes/Legacy.md"));

  REQUIRE_THROWS_AS(svc.create("notes/smart-pointers.md", input),
                    vault::DocumentAlreadyExistsError);
}

TEST_CASE("DocumentService::rename moves the file and its .assets folder, "
          "rewrites inbound wiki-links and asset hrefs, and keeps the index "
          "rowid (history) intact",
          "[DocumentService]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService svc(repo, indexUpdater, snapshots);
  index::NavQueries nav(db);

  vault::DocumentInput target;
  target.title = "Target";
  target.visibility = "public";
  target.body =
      "Self [[notes/foo]] and ![img](assets/notes/foo.assets/diagram.png)\n";
  svc.create("notes/foo.md", target);
  fs::create_directories(env.vaultRoot() / "notes/foo.assets");
  std::ofstream(env.vaultRoot() / "notes/foo.assets/diagram.png") << "fake";

  vault::DocumentInput linker;
  linker.title = "Linker";
  linker.visibility = "public";
  linker.body = "See [[notes/foo]] and [[notes/foo.md|the doc]] and "
                "also ![x](assets/notes/foo.assets/diagram.png).\n";
  svc.create("notes/linker.md", linker);

  vault::DocumentInput unrelated;
  unrelated.title = "Unrelated";
  unrelated.visibility = "public";
  unrelated.body = "See [[notes/food]] instead.\n";
  svc.create("notes/other.md", unrelated);

  const auto rowBefore = indexUpdater.rowIdForPath("notes/foo.md");
  REQUIRE(rowBefore.has_value());
  snapshots.record(*rowBefore, "pre-rename snapshot");

  const auto renamed = svc.rename("notes/foo", "archive/bar");
  REQUIRE(renamed.path == "archive/bar.md");
  REQUIRE(fs::exists(env.vaultRoot() / "archive/bar.md"));
  REQUIRE(fs::exists(env.vaultRoot() / "archive/bar.assets/diagram.png"));
  REQUIRE_FALSE(fs::exists(env.vaultRoot() / "notes/foo.md"));
  REQUIRE_FALSE(fs::exists(env.vaultRoot() / "notes/foo.assets/diagram.png"));

  REQUIRE(indexUpdater.rowIdForPath("archive/bar.md") == rowBefore);
  REQUIRE_FALSE(indexUpdater.rowIdForPath("notes/foo.md").has_value());
  REQUIRE(snapshots.list(*rowBefore).size() == 1);

  const auto moved = svc.get("archive/bar.md");
  REQUIRE(moved.body.find("[[archive/bar]]") != std::string::npos);
  REQUIRE(moved.body.find("assets/archive/bar.assets/diagram.png") != std::string::npos);
  REQUIRE(moved.body.find("assets/notes/foo.assets/") == std::string::npos);
  REQUIRE(moved.frontMatter.id == renamed.frontMatter.id);

  const auto linkerAfter = svc.get("notes/linker.md");
  REQUIRE(linkerAfter.body.find("[[archive/bar]]") != std::string::npos);
  REQUIRE(linkerAfter.body.find("[[archive/bar.md|the doc]]") != std::string::npos);
  REQUIRE(linkerAfter.body.find("[[notes/foo]]") == std::string::npos);
  REQUIRE(linkerAfter.body.find("assets/archive/bar.assets/diagram.png") !=
          std::string::npos);

  const auto otherAfter = svc.get("notes/other.md");
  REQUIRE(otherAfter.body == "See [[notes/food]] instead.\n");

  const auto backlinks = nav.backlinks("archive/bar.md", true);
  REQUIRE(backlinks.size() >= 1);
  bool foundLinker = false;
  for (const auto& b : backlinks) {
    if (b.path == "notes/linker.md") foundLinker = true;
    REQUIRE(b.path != "notes/foo.md");
  }
  REQUIRE(foundLinker);
  REQUIRE(nav.backlinks("notes/foo.md", true).empty());
}

TEST_CASE("DocumentService::rename rejects missing source, occupied dest, "
          "and is a no-op when source and dest normalize equal",
          "[DocumentService]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService svc(repo, indexUpdater, snapshots);

  vault::DocumentInput input;
  input.body = "x";
  svc.create("a.md", input);
  svc.create("b.md", input);

  REQUIRE_THROWS_AS(svc.rename("missing.md", "c.md"), vault::DocumentNotFoundError);
  REQUIRE_THROWS_AS(svc.rename("a.md", "b.md"), vault::DocumentAlreadyExistsError);

  const auto same = svc.rename("a.md", "a");
  REQUIRE(same.path == "a.md");
  REQUIRE(fs::exists(env.vaultRoot() / "a.md"));
}
