#include "index/Database.h"
#include "index/IndexUpdater.h"
#include "index/QueryBlocks.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace fs = std::filesystem;
using namespace wikicore::index;

namespace {

class TempDb {
 public:
  TempDb()
      : path_(fs::temp_directory_path() /
              fs::path("wiki-query-test-" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                        ".db")) {
    fs::remove(path_);
  }
  ~TempDb() { fs::remove(path_); }
  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

DocumentIndexEntry makeEntry(std::string path, std::string visibility,
                              std::vector<std::string> tags, std::string docType = "",
                              std::string updatedAt = "2026-01-01T00:00:00Z") {
  DocumentIndexEntry e;
  e.uuid = path;
  e.path = std::move(path);
  e.title = e.path;
  e.visibility = std::move(visibility);
  e.createdAt = "2026-01-01T00:00:00Z";
  e.updatedAt = std::move(updatedAt);
  e.tags = std::move(tags);
  e.docType = std::move(docType);
  return e;
}

bool hasPath(const std::vector<QueryResultRow>& rows, const std::string& path) {
  for (const auto& r : rows) {
    if (r.path == path) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("QueryBlocks: empty query lists all visible documents, visibility-gated",
          "[QueryBlocks]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "public", {}));
  updater.upsertOne(makeEntry("b.md", "private", {}));

  QueryBlocks qb(database);
  auto anon = qb.parseAndRun("", false);
  REQUIRE(anon.ok);
  REQUIRE(anon.rows.size() == 1);
  REQUIRE(anon.rows[0].path == "a.md");

  auto admin = qb.parseAndRun("", true);
  REQUIRE(admin.ok);
  REQUIRE(admin.rows.size() == 2);
}

TEST_CASE("QueryBlocks: tag filter requires ALL listed tags (AND, not OR)",
          "[QueryBlocks]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("both.md", "public", {"cpp", "cheatsheet"}));
  updater.upsertOne(makeEntry("cpp-only.md", "public", {"cpp"}));
  updater.upsertOne(makeEntry("cheatsheet-only.md", "public", {"cheatsheet"}));

  QueryBlocks qb(database);
  auto result = qb.parseAndRun("tag: cpp, cheatsheet", true);
  REQUIRE(result.ok);
  REQUIRE(result.rows.size() == 1);
  REQUIRE(result.rows[0].path == "both.md");
}

TEST_CASE("QueryBlocks: type filter matches exactly, excludes everything else",
          "[QueryBlocks]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "public", {}, "recipe"));
  updater.upsertOne(makeEntry("b.md", "public", {}, "note"));

  QueryBlocks qb(database);
  auto result = qb.parseAndRun("type: recipe", true);
  REQUIRE(result.ok);
  REQUIRE(result.rows.size() == 1);
  REQUIRE(result.rows[0].path == "a.md");
}

TEST_CASE("QueryBlocks: folder filter is a path PREFIX match, not substring",
          "[QueryBlocks]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("recipes/dinner/borscht.md", "public", {}));
  updater.upsertOne(makeEntry("recipes/breakfast/pancakes.md", "public", {}));
  updater.upsertOne(makeEntry("notes/recipes-are-not-here.md", "public", {}));

  QueryBlocks qb(database);
  auto result = qb.parseAndRun("folder: recipes/", true);
  REQUIRE(result.ok);
  REQUIRE(result.rows.size() == 2);
  REQUIRE(!hasPath(result.rows, "notes/recipes-are-not-here.md"));
}

TEST_CASE("QueryBlocks: a folder value containing LIKE wildcard characters "
          "is matched literally, not as a wildcard",
          "[QueryBlocks]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a%b/doc.md", "public", {}));
  updater.upsertOne(makeEntry("axxb/doc.md", "public", {}));  // would match if % were a wildcard

  QueryBlocks qb(database);
  auto result = qb.parseAndRun("folder: a%b/", true);
  REQUIRE(result.ok);
  REQUIRE(result.rows.size() == 1);
  REQUIRE(result.rows[0].path == "a%b/doc.md");
}

TEST_CASE("QueryBlocks: sort/order with explicit values", "[QueryBlocks]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("z.md", "public", {}, "", "2026-03-01T00:00:00Z"));
  updater.upsertOne(makeEntry("a.md", "public", {}, "", "2026-01-01T00:00:00Z"));

  QueryBlocks qb(database);
  auto byUpdatedDesc = qb.parseAndRun("sort: updated", true);  // default desc
  REQUIRE(byUpdatedDesc.ok);
  REQUIRE(byUpdatedDesc.rows.size() == 2);
  REQUIRE(byUpdatedDesc.rows[0].path == "z.md");

  auto byUpdatedAsc = qb.parseAndRun("sort: updated\norder: asc", true);
  REQUIRE(byUpdatedAsc.ok);
  REQUIRE(byUpdatedAsc.rows[0].path == "a.md");

  auto byTitleDefault = qb.parseAndRun("sort: title", true);  // default asc
  REQUIRE(byTitleDefault.ok);
  REQUIRE(byTitleDefault.rows[0].path == "a.md");
}

TEST_CASE("QueryBlocks: limit clamps result count and rejects out-of-range values",
          "[QueryBlocks]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  for (int i = 0; i < 5; ++i) {
    updater.upsertOne(makeEntry("doc" + std::to_string(i) + ".md", "public", {}));
  }

  QueryBlocks qb(database);
  auto limited = qb.parseAndRun("limit: 2", true);
  REQUIRE(limited.ok);
  REQUIRE(limited.rows.size() == 2);

  REQUIRE_FALSE(qb.parseAndRun("limit: 0", true).ok);
  REQUIRE_FALSE(qb.parseAndRun("limit: 101", true).ok);
  REQUIRE_FALSE(qb.parseAndRun("limit: not-a-number", true).ok);
}

TEST_CASE("QueryBlocks: orphans finds documents with zero VISIBLE incoming "
          "backlinks, gated the same fail-safe-private way as NavQueries::backlinks",
          "[QueryBlocks]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);

  auto withBody = [](DocumentIndexEntry e, std::string body) {
    e.body = std::move(body);
    return e;
  };
  updater.upsertOne(makeEntry("linked.md", "public", {}));
  updater.upsertOne(makeEntry("orphan.md", "public", {}));
  // "linked.md" only has a PRIVATE linker -- an anon caller can't see
  // that link, so it must still count as orphaned for them, while an
  // admin (who CAN see the private linker) must not see it as orphaned.
  updater.upsertOne(
      withBody(makeEntry("secret-linker.md", "private", {}), "[[linked]]"));

  QueryBlocks qb(database);
  auto anonOrphans = qb.parseAndRun("orphans: true", false);
  REQUIRE(anonOrphans.ok);
  REQUIRE(hasPath(anonOrphans.rows, "orphan.md"));
  REQUIRE(hasPath(anonOrphans.rows, "linked.md"));  // private link invisible to anon

  auto adminOrphans = qb.parseAndRun("orphans: true", true);
  REQUIRE(adminOrphans.ok);
  REQUIRE(hasPath(adminOrphans.rows, "orphan.md"));
  REQUIRE(!hasPath(adminOrphans.rows, "linked.md"));  // admin sees the private link
}

TEST_CASE("QueryBlocks: an unknown key is a parse error, not a silently "
          "unfiltered or empty result",
          "[QueryBlocks]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "public", {}));

  QueryBlocks qb(database);
  auto result = qb.parseAndRun("tags: cpp", true);  // typo: "tags" not "tag"
  REQUIRE_FALSE(result.ok);
  REQUIRE(result.error.find("unknown key") != std::string::npos);
}

TEST_CASE("QueryBlocks: a duplicate key is a parse error, not last-write-wins",
          "[QueryBlocks]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);

  QueryBlocks qb(database);
  REQUIRE_FALSE(qb.parseAndRun("type: a\ntype: b", true).ok);
  REQUIRE_FALSE(qb.parseAndRun("tag: a\ntag: b", true).ok);
}

TEST_CASE("QueryBlocks: an invalid sort/order value is a parse error",
          "[QueryBlocks]") {
  TempDb db;
  Database database(db.path());
  database.migrate();

  QueryBlocks qb(database);
  REQUIRE_FALSE(qb.parseAndRun("sort: nonsense", true).ok);
  REQUIRE_FALSE(qb.parseAndRun("order: sideways", true).ok);
}

TEST_CASE("QueryBlocks: a tag value that looks like a SQL injection attempt "
          "is treated as a literal, inert tag name",
          "[QueryBlocks]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "public", {"real-tag"}));

  QueryBlocks qb(database);
  // A tag name this weird can never legitimately exist -- proves the
  // value only ever reaches SQLite as a bound parameter (an exact,
  // literal match attempt), never concatenated into the query text
  // itself, which would either syntax-error or actually drop something.
  auto result = qb.parseAndRun("tag: x'; DROP TABLE documents; --", true);
  REQUIRE(result.ok);
  REQUIRE(result.rows.empty());

  // The documents table must still exist and work normally afterward.
  auto sane = qb.parseAndRun("", true);
  REQUIRE(sane.ok);
  REQUIRE(sane.rows.size() == 1);
}
