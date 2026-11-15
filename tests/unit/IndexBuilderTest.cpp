#include "index/Database.h"
#include "index/FtsSearch.h"
#include "index/IndexBuilder.h"
#include "index/IndexUpdater.h"
#include "vault/FrontMatter.h"
#include "vault/VaultRepository.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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

  std::string readFile(const std::string& relPath) const {
    std::ifstream in(vaultRoot() / relPath, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
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

TEST_CASE("IndexBuilder never indexes a markdown file inside an attachments folder",
          "[IndexBuilder]") {
  TempEnv env;
  env.writeFile("notes/priv.md", "---\ntitle: Priv\nvisibility: private\n---\nbody");
  env.writeFile("notes/priv.assets/note.md",
                "---\ntitle: Smuggled\nvisibility: public\n---\nattachment body");

  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater updater(db);
  vault::VaultRepository vault(env.vaultRoot());
  index::IndexBuilder builder(vault, updater);

  REQUIRE(builder.fullRescan().documentsIndexed == 1);
  REQUIRE_FALSE(builder.reindexOneFile("notes/priv.assets/note.md"));
  REQUIRE(updater.allIndexedPaths() == std::vector<std::string>{"notes/priv.md"});
}

TEST_CASE("IndexBuilder::fullRescan sweeps an attachment indexed by an older version",
          "[IndexBuilder]") {
  TempEnv env;
  env.writeFile("notes/priv.assets/note.md", "---\nvisibility: public\n---\nattachment body");

  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater updater(db);
  index::DocumentIndexEntry stale;
  stale.uuid = "11111111-1111-4111-8111-111111111111";
  stale.path = "notes/priv.assets/note.md";
  stale.visibility = "public";
  updater.upsertOne(stale);

  vault::VaultRepository vault(env.vaultRoot());
  index::IndexBuilder builder(vault, updater);
  REQUIRE(builder.fullRescan().staleRowsRemoved == 1);
  REQUIRE(updater.allIndexedPaths().empty());
}

namespace {

constexpr const char* kSharedId = "11111111-2222-4333-8444-555555555555";

std::string documentWithSharedId(const std::string& title) {
  return std::string("---\nid: ") + kSharedId + "\ntitle: " + title +
         "\naliases: [kept-verbatim]\nvisibility: public\n---\nbody of " + title + "\n";
}

std::string idOf(const std::string& raw) { return vault::parseFrontMatter(raw).frontMatter.id; }

}  // namespace

TEST_CASE("IndexBuilder::fullRescan gives a copied document a new id of its own",
          "[IndexBuilder]") {
  TempEnv env;
  const std::string original = documentWithSharedId("Copy");
  env.writeFile("a.md", original);
  env.writeFile("b.md", original);

  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater updater(db);
  vault::VaultRepository vault(env.vaultRoot());
  index::IndexBuilder builder(vault, updater);

  const auto stats = builder.fullRescan();
  REQUIRE(stats.documentsIndexed == 2);

  const std::string a = env.readFile("a.md");
  const std::string b = env.readFile("b.md");
  const bool aKeptId = idOf(a) == kSharedId;
  const std::string& kept = aKeptId ? a : b;
  const std::string& healed = aKeptId ? b : a;
  const std::string healedPath = aKeptId ? "b.md" : "a.md";
  REQUIRE(kept == original);
  const std::string newId = idOf(healed);
  REQUIRE(!newId.empty());
  REQUIRE(newId != kSharedId);

  std::string expectedHealed = original;
  const std::string idLine = std::string("id: ") + kSharedId;
  expectedHealed.replace(expectedHealed.find(idLine), idLine.size(), "id: " + newId);
  REQUIRE(healed == expectedHealed);

  REQUIRE(updater.findPathByUuid(kSharedId) == std::string(aKeptId ? "a.md" : "b.md"));
  REQUIRE(updater.findPathByUuid(newId) == healedPath);

  builder.fullRescan();
  REQUIRE(env.readFile("a.md") == a);
  REQUIRE(env.readFile("b.md") == b);
}

TEST_CASE("IndexBuilder::fullRescan keeps the id of a document renamed while "
          "nothing was watching",
          "[IndexBuilder]") {
  TempEnv env;
  const std::string original = documentWithSharedId("Moved");
  env.writeFile("a.md", original);

  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater updater(db);
  vault::VaultRepository vault(env.vaultRoot());
  index::IndexBuilder builder(vault, updater);
  builder.fullRescan();
  const auto rowBeforeRename = updater.rowIdForPath("a.md");
  REQUIRE(rowBeforeRename.has_value());

  fs::rename(env.vaultRoot() / "a.md", env.vaultRoot() / "b.md");
  const auto stats = builder.fullRescan();

  REQUIRE(stats.documentsIndexed == 1);
  REQUIRE(env.readFile("b.md") == original);
  REQUIRE(updater.findPathByUuid(kSharedId) == std::string("b.md"));
  REQUIRE(updater.allIndexedPaths() == std::vector<std::string>{"b.md"});
  REQUIRE(updater.rowIdForPath("b.md") == rowBeforeRename);
}

TEST_CASE("IndexBuilder::reindexOneFile gives a document copied in while the "
          "server runs a new id of its own",
          "[IndexBuilder]") {
  TempEnv env;
  const std::string original = documentWithSharedId("Live");
  env.writeFile("a.md", original);

  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater updater(db);
  vault::VaultRepository vault(env.vaultRoot());
  index::IndexBuilder builder(vault, updater);
  builder.fullRescan();

  env.writeFile("b.md", original);
  REQUIRE(builder.reindexOneFile("b.md"));

  REQUIRE(env.readFile("a.md") == original);
  const std::string newId = idOf(env.readFile("b.md"));
  REQUIRE(!newId.empty());
  REQUIRE(newId != kSharedId);
  REQUIRE(updater.findPathByUuid(kSharedId) == std::string("a.md"));
  REQUIRE(updater.findPathByUuid(newId) == std::string("b.md"));
}

TEST_CASE("IndexBuilder::fullRescan follows two documents that swapped ids",
          "[IndexBuilder]") {
  TempEnv env;
  const std::string otherId = "99999999-2222-4333-8444-555555555555";
  auto withId = [](const std::string& id, const std::string& title) {
    return "---\nid: " + id + "\ntitle: " + title + "\n---\nbody\n";
  };
  env.writeFile("a.md", withId(kSharedId, "A"));
  env.writeFile("b.md", withId(otherId, "B"));

  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater updater(db);
  vault::VaultRepository vault(env.vaultRoot());
  index::IndexBuilder builder(vault, updater);
  builder.fullRescan();
  const auto rowA = updater.rowIdForPath("a.md");
  const auto rowB = updater.rowIdForPath("b.md");

  env.writeFile("a.md", withId(otherId, "A"));
  env.writeFile("b.md", withId(kSharedId, "B"));
  const auto stats = builder.fullRescan();

  REQUIRE(stats.documentsIndexed == 2);
  REQUIRE(env.readFile("a.md") == withId(otherId, "A"));
  REQUIRE(env.readFile("b.md") == withId(kSharedId, "B"));
  REQUIRE(updater.findPathByUuid(kSharedId) == std::string("b.md"));
  REQUIRE(updater.findPathByUuid(otherId) == std::string("a.md"));
  REQUIRE(updater.rowIdForPath("a.md") == rowA);
  REQUIRE(updater.rowIdForPath("b.md") == rowB);
}

TEST_CASE("IndexBuilder::fullRescan indexes a duplicate id it cannot rewrite in place",
          "[IndexBuilder]") {
  TempEnv env;
  env.writeFile("a.md", documentWithSharedId("Block style"));
  const std::string flowStyle =
      std::string("---\n{id: ") + kSharedId + ", title: Flow style, visibility: public}\n---\nbody\n";
  env.writeFile("b.md", flowStyle);

  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater updater(db);
  vault::VaultRepository vault(env.vaultRoot());
  index::IndexBuilder builder(vault, updater);

  const auto stats = builder.fullRescan();
  REQUIRE(stats.documentsIndexed == 2);
  REQUIRE(env.readFile("b.md") == flowStyle);

  index::FtsSearch search(db);
  index::SearchQuery q;
  q.includePrivate = true;
  REQUIRE(search.search(q).size() == 2);
}

TEST_CASE("IndexUpdater::upsertOne refuses a uuid indexed for another path and "
          "leaves the index as it was",
          "[IndexBuilder]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater updater(db);

  index::DocumentIndexEntry entry;
  entry.uuid = kSharedId;
  entry.path = "a.md";
  entry.title = "A";
  entry.visibility = "public";
  entry.body = "body";
  updater.upsertOne(entry);

  entry.path = "b.md";
  REQUIRE_THROWS_AS(updater.upsertOne(entry), index::UuidTakenError);
  REQUIRE(updater.allIndexedPaths() == std::vector<std::string>{"a.md"});

  entry.uuid = "99999999-2222-4333-8444-555555555555";
  updater.upsertOne(entry);
  REQUIRE(updater.findPathByUuid(entry.uuid) == std::string("b.md"));
  REQUIRE(updater.findPathByUuid(kSharedId) == std::string("a.md"));
}
