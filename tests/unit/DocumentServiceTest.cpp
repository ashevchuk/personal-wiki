#include "index/Database.h"
#include "index/IndexUpdater.h"
#include "index/NavQueries.h"
#include "index/SnapshotStore.h"
#include "vault/DocumentService.h"

#include "vault/PathGuard.h"

#include <catch2/catch_test_macros.hpp>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

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

TEST_CASE("DocumentService update gives a document whose id another file "
          "holds a new id of its own",
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
  input.body = "v1";
  const auto original = svc.create("a.md", input);
  fs::copy_file(env.vaultRoot() / "a.md", env.vaultRoot() / "b.md");

  input.title = "Copy";
  input.body = "v2";
  const auto updated = svc.update("b.md", input);

  REQUIRE(!updated.frontMatter.id.empty());
  REQUIRE(updated.frontMatter.id != original.frontMatter.id);
  REQUIRE(svc.get("b.md").frontMatter.id == updated.frontMatter.id);
  REQUIRE(svc.get("a.md").frontMatter.id == original.frontMatter.id);
  REQUIRE(indexUpdater.findPathByUuid(original.frontMatter.id) == std::string("a.md"));
  REQUIRE(indexUpdater.findPathByUuid(updated.frontMatter.id) == std::string("b.md"));
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

TEST_CASE("VaultRepository::listTrash finds a soft-deleted document, "
          "purgeFromTrash removes it (and its assets folder) for good",
          "[VaultRepository]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService svc(repo, indexUpdater, snapshots);

  REQUIRE(repo.listTrash().empty());

  vault::DocumentInput input;
  input.body = "x";
  svc.create("notes/a.md", input);
  fs::create_directories(env.vaultRoot() / "notes/a.assets");
  std::ofstream(env.vaultRoot() / "notes/a.assets/img.png") << "fake";
  svc.softDelete("notes/a.md");

  const auto trashed = repo.listTrash();
  REQUIRE(trashed.size() == 1);
  REQUIRE(trashed[0].relativePath == "notes/a.md");
  REQUIRE(trashed[0].sizeBytes > 0);
  // The assets folder itself is not a separate row -- same as it isn't
  // one in any real vault listing either.

  repo.purgeFromTrash("notes/a.md");
  REQUIRE(repo.listTrash().empty());
  REQUIRE_FALSE(fs::exists(env.vaultRoot() / ".trash/notes/a.md"));
  REQUIRE_FALSE(fs::exists(env.vaultRoot() / ".trash/notes/a.assets"));
  REQUIRE_THROWS_AS(repo.purgeFromTrash("notes/a.md"), fs::filesystem_error);
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

TEST_CASE("DocumentService refuses paths with a dot-prefixed segment", "[DocumentService]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService svc(repo, indexUpdater, snapshots);

  vault::DocumentInput input;
  input.title = "Doc";
  input.visibility = "public";
  svc.create("notes/a.md", input);
  svc.softDelete("notes/a.md");
  REQUIRE(fs::exists(env.vaultRoot() / ".trash/notes/a.md"));

  REQUIRE_THROWS_AS(svc.get(".trash/notes/a.md"), vault::PathTraversalError);
  REQUIRE_THROWS_AS(svc.update(".trash/notes/a.md", input), vault::PathTraversalError);
  REQUIRE_THROWS_AS(svc.softDelete(".trash/notes/a.md"), vault::PathTraversalError);
  REQUIRE_THROWS_AS(svc.create(".hidden/b.md", input), vault::PathTraversalError);
  REQUIRE_THROWS_AS(svc.create("notes/.b", input), vault::PathTraversalError);
  REQUIRE_THROWS_AS(svc.rename(".trash/notes/a.md", "notes/a.md"), vault::PathTraversalError);
  svc.create("notes/c.md", input);
  REQUIRE_THROWS_AS(svc.rename("notes/c.md", ".trash/c.md"), vault::PathTraversalError);
  REQUIRE(fs::exists(env.vaultRoot() / "notes/c.md"));
}

TEST_CASE("DocumentService::rename gives a rewritten linker without an id of "
          "its own a new one",
          "[DocumentService]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService svc(repo, indexUpdater, snapshots);

  vault::DocumentInput input;
  input.title = "Target";
  input.body = "target";
  svc.create("target.md", input);
  input.title = "Linker";
  input.body = "see [[target]]";
  const auto linker = svc.create("linker.md", input);
  svc.create("copy.md", input);
  svc.create("no-id-1.md", input);
  svc.create("no-id-2.md", input);

  // Overwritten on disk but not yet re-indexed: copy.md now carries
  // linker.md's id, and the no-id files carry none.
  fs::copy_file(env.vaultRoot() / "linker.md", env.vaultRoot() / "copy.md",
                fs::copy_options::overwrite_existing);
  for (const char* path : {"no-id-1.md", "no-id-2.md"}) {
    std::ofstream(env.vaultRoot() / path) << "---\ntitle: No id\n---\nsee [[target]]";
  }

  svc.rename("target.md", "moved.md");

  std::vector<std::string> ids;
  for (const char* path : {"linker.md", "copy.md", "no-id-1.md", "no-id-2.md"}) {
    CAPTURE(path);
    const auto rec = svc.get(path);
    REQUIRE(rec.body.find("[[moved]]") != std::string::npos);
    REQUIRE(!rec.frontMatter.id.empty());
    REQUIRE(indexUpdater.findPathByUuid(rec.frontMatter.id) == std::string(path));
    ids.push_back(rec.frontMatter.id);
  }
  REQUIRE(ids[0] == linker.frontMatter.id);
  std::sort(ids.begin(), ids.end());
  REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}

TEST_CASE("DocumentService update keeps front-matter keys it does not model",
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
  input.body = "v1";
  const auto created = svc.create("a.md", input);
  std::ofstream(env.vaultRoot() / "a.md")
      << "---\nid: " << created.frontMatter.id
      << "\ntitle: Original\naliases: [one, two]\nmeta:\n  source: obsidian\n---\nv1";

  input.title = "Changed";
  input.body = "v2";
  svc.update("a.md", input);

  std::ifstream in(env.vaultRoot() / "a.md", std::ios::binary);
  const std::string raw(std::istreambuf_iterator<char>(in), {});
  const YAML::Node block = YAML::Load(raw.substr(4, raw.find("\n---\n", 4) - 4));
  REQUIRE(block["title"].as<std::string>() == "Changed");
  REQUIRE(block["id"].as<std::string>() == created.frontMatter.id);
  REQUIRE(block["aliases"].as<std::vector<std::string>>() ==
          std::vector<std::string>{"one", "two"});
  REQUIRE(block["meta"]["source"].as<std::string>() == "obsidian");
  REQUIRE(svc.get("a.md").body == "v2");
}

TEST_CASE("DocumentService::rename rewrites only the body of a linker",
          "[DocumentService]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService svc(repo, indexUpdater, snapshots);

  vault::DocumentInput input;
  input.title = "Target";
  input.body = "target";
  svc.create("target.md", input);
  input.title = "Linker";
  input.body = "see [[target]]";
  const auto linker = svc.create("linker.md", input);
  svc.create("no-id.md", input);

  const std::string linkerFrontMatter =
      "---\r\n# written by hand\r\nid: " + linker.frontMatter.id +
      "\r\ntitle: Linker\r\naliases: [one, two]\r\nvisibility: public\r\n---\r\n";
  std::ofstream(env.vaultRoot() / "linker.md", std::ios::binary)
      << linkerFrontMatter << "see [[target]]\r\n";
  std::ofstream(env.vaultRoot() / "no-id.md", std::ios::binary)
      << "---\ntitle: No id\naliases: [a]\n---\nsee [[target]]\n";

  svc.rename("target.md", "moved.md");

  auto readFile = [&](const char* path) {
    std::ifstream in(env.vaultRoot() / path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  };
  REQUIRE(readFile("linker.md") == linkerFrontMatter + "see [[moved]]\r\n");

  const std::string noIdId = svc.get("no-id.md").frontMatter.id;
  REQUIRE(!noIdId.empty());
  REQUIRE(readFile("no-id.md") ==
          "---\nid: " + noIdId + "\ntitle: No id\naliases: [a]\n---\nsee [[moved]]\n");
  REQUIRE(indexUpdater.findPathByUuid(noIdId) == std::string("no-id.md"));
}
