#include "index/Database.h"
#include "index/FtsSearch.h"
#include "index/IndexBuilder.h"
#include "index/IndexUpdater.h"
#include "vault/VaultRepository.h"

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
              fs::path("wiki-indexbuilder-test-" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this)))) {
    fs::remove_all(root_);
    fs::create_directories(root_ / "vault");
  }
  ~TempEnv() { fs::remove_all(root_); }
  TempEnv(const TempEnv&) = delete;
  TempEnv& operator=(const TempEnv&) = delete;

  fs::path vaultRoot() const { return root_ / "vault"; }
  fs::path dbPath() const { return root_ / "index.db"; }

  void writeFile(const std::string& relPath, const std::string& content) const {
    const fs::path full = vaultRoot() / relPath;
    fs::create_directories(full.parent_path());
    std::ofstream(full) << content;
  }

 private:
  fs::path root_;
};

}  // namespace

TEST_CASE("IndexBuilder::fullRescan indexes every .md file, skipping dotdirs",
          "[IndexBuilder]") {
  TempEnv env;
  env.writeFile("notes/a.md", "---\ntitle: A\nvisibility: public\n---\nbody a");
  env.writeFile("notes/sub/b.md", "---\nvisibility: public\n---\nbody b");
  env.writeFile(".trash/old.md", "---\nvisibility: public\n---\nshould be skipped");
  env.writeFile("not-markdown.txt", "irrelevant");

  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater updater(db);
  vault::VaultRepository vault(env.vaultRoot());
  index::IndexBuilder builder(vault, updater);

  const auto stats = builder.fullRescan();
  REQUIRE(stats.documentsIndexed == 2);
  REQUIRE(stats.staleRowsRemoved == 0);

  index::FtsSearch search(db);
  index::SearchQuery q;
  q.includePrivate = true;
  const auto all = search.search(q);
  REQUIRE(all.size() == 2);
}

TEST_CASE("IndexBuilder::fullRescan falls back to the filename for a "
          "missing title",
          "[IndexBuilder]") {
  TempEnv env;
  env.writeFile("no-title.md", "---\nvisibility: public\n---\nbody, no title field");

  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater updater(db);
  vault::VaultRepository vault(env.vaultRoot());
  index::IndexBuilder builder(vault, updater);
  builder.fullRescan();

  index::FtsSearch search(db);
  index::SearchQuery q;
  q.includePrivate = true;
  const auto results = search.search(q);
  REQUIRE(results.size() == 1);
  REQUIRE(results[0].title == "no-title");
}

TEST_CASE("IndexBuilder::fullRescan sweeps a row whose file was deleted "
          "outside the app",
          "[IndexBuilder]") {
  TempEnv env;
  env.writeFile("gone.md", "---\nvisibility: public\n---\nwill be deleted");

  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater updater(db);
  vault::VaultRepository vault(env.vaultRoot());
  index::IndexBuilder builder(vault, updater);

  REQUIRE(builder.fullRescan().documentsIndexed == 1);

  fs::remove(env.vaultRoot() / "gone.md");
  const auto secondPass = builder.fullRescan();
  REQUIRE(secondPass.documentsIndexed == 0);
  REQUIRE(secondPass.staleRowsRemoved == 1);
}
