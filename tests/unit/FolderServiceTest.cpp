#include "index/Database.h"
#include "index/IndexBuilder.h"
#include "index/IndexUpdater.h"
#include "index/SnapshotStore.h"
#include "vault/DocumentService.h"
#include "vault/FolderService.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

namespace fs = std::filesystem;
using namespace wikicore;

namespace {

// RAII temp vault + index db per test, mirroring DocumentServiceTest's
// TempEnv.
class TempEnv {
 public:
  TempEnv()
      : root_(fs::temp_directory_path() /
              fs::path("wiki-foldersvc-test-" +
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

TEST_CASE("FolderService move relocates a whole subtree and reindexes every "
          "document under it at its new path",
          "[FolderService]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  index::IndexBuilder indexBuilder(repo, indexUpdater);
  vault::DocumentService docSvc(repo, indexUpdater, snapshots);
  vault::FolderService folderSvc(repo, indexUpdater, indexBuilder);

  vault::DocumentInput input;
  input.title = "Deep note";
  input.tags = {"x"};
  input.visibility = "public";
  input.body = "body";
  const auto created = docSvc.create("notes/sub/deep.md", input);
  fs::create_directories(env.vaultRoot() / "notes/sub/deep.assets");
  std::ofstream(env.vaultRoot() / "notes/sub/deep.assets/img.png") << "fake";

  const int64_t reindexed = folderSvc.move("notes", "archive/notes");
  REQUIRE(reindexed == 1);

  // The whole subtree, including the nested .assets folder, actually
  // moved on disk.
  REQUIRE_FALSE(fs::exists(env.vaultRoot() / "notes"));
  REQUIRE(fs::exists(env.vaultRoot() / "archive/notes/sub/deep.md"));
  REQUIRE(fs::exists(env.vaultRoot() / "archive/notes/sub/deep.assets/img.png"));

  // The index reflects the NEW path, front matter (id/title/tags)
  // untouched, and the OLD path is gone — not a stale duplicate.
  const auto atNewPath = docSvc.get("archive/notes/sub/deep.md");
  REQUIRE(atNewPath.frontMatter.id == created.frontMatter.id);
  REQUIRE(atNewPath.frontMatter.title == "Deep note");
  REQUIRE(atNewPath.frontMatter.tags == std::vector<std::string>{"x"});
  REQUIRE(indexUpdater.findPathByUuid(created.frontMatter.id) ==
          std::optional<std::string>("archive/notes/sub/deep.md"));

  const auto indexedPaths = indexUpdater.allIndexedPaths();
  REQUIRE(std::find(indexedPaths.begin(), indexedPaths.end(), "notes/sub/deep.md") ==
          indexedPaths.end());
}

TEST_CASE("FolderService move rejects missing source, occupied destination, "
          "moving into itself, and empty paths",
          "[FolderService]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  index::IndexBuilder indexBuilder(repo, indexUpdater);
  vault::DocumentService docSvc(repo, indexUpdater, snapshots);
  vault::FolderService folderSvc(repo, indexUpdater, indexBuilder);

  REQUIRE_THROWS_AS(folderSvc.move("does-not-exist", "elsewhere"),
                     vault::FolderNotFoundError);

  vault::DocumentInput input;
  input.body = "x";
  docSvc.create("a/one.md", input);
  docSvc.create("b/two.md", input);
  REQUIRE_THROWS_AS(folderSvc.move("a", "b"), vault::FolderAlreadyExistsError);

  REQUIRE_THROWS_AS(folderSvc.move("a", "a/nested"), vault::InvalidFolderMoveError);
  REQUIRE_THROWS_AS(folderSvc.move("a", "a"), vault::InvalidFolderMoveError);
  REQUIRE_THROWS_AS(folderSvc.move("", "somewhere"), vault::InvalidFolderMoveError);
}

TEST_CASE("FolderService remove requires an existing, empty directory",
          "[FolderService]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  index::IndexBuilder indexBuilder(repo, indexUpdater);
  vault::DocumentService docSvc(repo, indexUpdater, snapshots);
  vault::FolderService folderSvc(repo, indexUpdater, indexBuilder);

  REQUIRE_THROWS_AS(folderSvc.remove("nope"), vault::FolderNotFoundError);

  fs::create_directories(env.vaultRoot() / "empty-one");
  REQUIRE(folderSvc.isEmpty("empty-one"));
  folderSvc.remove("empty-one");
  REQUIRE_FALSE(fs::exists(env.vaultRoot() / "empty-one"));

  vault::DocumentInput input;
  input.body = "x";
  docSvc.create("occupied/doc.md", input);
  REQUIRE_FALSE(folderSvc.isEmpty("occupied"));
  REQUIRE_THROWS_AS(folderSvc.remove("occupied"), vault::FolderNotEmptyError);
}
