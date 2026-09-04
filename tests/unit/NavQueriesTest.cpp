#include "index/Database.h"
#include "index/IndexUpdater.h"
#include "index/NavQueries.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace fs = std::filesystem;
using namespace wikicore::index;

namespace {

class TempDb {
 public:
  TempDb()
      : path_(fs::temp_directory_path() /
              fs::path("wiki-nav-test-" +
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
                              std::vector<std::string> tags) {
  DocumentIndexEntry e;
  e.uuid = path;
  e.path = std::move(path);
  e.title = e.path;
  e.visibility = std::move(visibility);
  e.createdAt = e.updatedAt = "2026-01-01T00:00:00Z";
  e.tags = std::move(tags);
  return e;
}

}  // namespace

TEST_CASE("NavQueries::listVisibleDocuments hides private docs for anon",
          "[NavQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "public", {}));
  updater.upsertOne(makeEntry("b.md", "private", {}));

  NavQueries nav(database);
  REQUIRE(nav.listVisibleDocuments(false).size() == 1);
  REQUIRE(nav.listVisibleDocuments(true).size() == 2);
}

TEST_CASE("NavQueries::tagCounts does not leak a count for a tag used only "
          "by private documents",
          "[NavQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "public", {"shared"}));
  updater.upsertOne(makeEntry("b.md", "private", {"shared", "secret-only"}));

  NavQueries nav(database);

  const auto anonTags = nav.tagCounts(false);
  auto findCount = [](const std::vector<TagCount>& tags, const std::string& name) {
    for (const auto& t : tags) {
      if (t.tag == name) return t.count;
    }
    return static_cast<int64_t>(-1);
  };

  REQUIRE(findCount(anonTags, "shared") == 1);
  REQUIRE(findCount(anonTags, "secret-only") == -1);  // absent, not zero

  const auto adminTags = nav.tagCounts(true);
  REQUIRE(findCount(adminTags, "shared") == 2);
  REQUIRE(findCount(adminTags, "secret-only") == 1);
}

TEST_CASE("NavQueries::tagCounts sorts alphabetically, case-insensitively, "
          "not by count",
          "[NavQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  // "zebra" carries the most documents but must still sort LAST -- a
  // count-desc order (the old default) would have put it first.
  // "Apple"/"banana" differ in case from a real-world un-normalized tag
  // (nothing in this app lowercases tags on save) -- COLLATE NOCASE is
  // what keeps "Apple" from sorting after every lowercase tag via plain
  // ASCII byte order.
  updater.upsertOne(makeEntry("a.md", "public", {"zebra"}));
  updater.upsertOne(makeEntry("b.md", "public", {"zebra"}));
  updater.upsertOne(makeEntry("c.md", "public", {"zebra"}));
  updater.upsertOne(makeEntry("d.md", "public", {"Apple"}));
  updater.upsertOne(makeEntry("e.md", "public", {"banana"}));

  NavQueries nav(database);
  const auto tags = nav.tagCounts(true);
  REQUIRE(tags.size() == 3);
  REQUIRE(tags[0].tag == "Apple");
  REQUIRE(tags[1].tag == "banana");
  REQUIRE(tags[2].tag == "zebra");
}

TEST_CASE("NavQueries::typeCounts excludes untyped docs and leaks no count "
          "for a type used only by private documents",
          "[NavQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);

  auto withType = [](DocumentIndexEntry e, std::string type) {
    e.docType = std::move(type);
    return e;
  };
  updater.upsertOne(withType(makeEntry("a.md", "public", {}), "recipe"));
  updater.upsertOne(withType(makeEntry("b.md", "private", {}), "secret-type"));
  updater.upsertOne(makeEntry("c.md", "public", {}));  // docType left "" -> excluded

  NavQueries nav(database);
  auto findCount = [](const std::vector<TagCount>& types, const std::string& name) {
    for (const auto& t : types) {
      if (t.tag == name) return t.count;
    }
    return static_cast<int64_t>(-1);
  };

  const auto anonTypes = nav.typeCounts(false);
  REQUIRE(findCount(anonTypes, "recipe") == 1);
  REQUIRE(findCount(anonTypes, "secret-type") == -1);  // absent, not zero
  REQUIRE(findCount(anonTypes, "") == -1);              // untyped never appears

  const auto adminTypes = nav.typeCounts(true);
  REQUIRE(findCount(adminTypes, "secret-type") == 1);
}

TEST_CASE("NavQueries::backlinks finds documents linking here via "
          "[[wiki-links]] and leaks nothing from a private linker",
          "[NavQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);

  auto withBody = [](DocumentIndexEntry e, std::string body) {
    e.body = std::move(body);
    return e;
  };
  // Two public docs link to target.md, one private doc also links to it,
  // and one doc's link points somewhere else entirely (must never appear).
  updater.upsertOne(makeEntry("target.md", "public", {}));
  updater.upsertOne(withBody(makeEntry("a.md", "public", {}), "See [[target]]."));
  updater.upsertOne(withBody(makeEntry("b.md", "public", {}), "Also [[target.md]]."));
  updater.upsertOne(withBody(makeEntry("secret.md", "private", {}), "[[target]] too."));
  updater.upsertOne(withBody(makeEntry("c.md", "public", {}), "[[somewhere/else]]."));

  NavQueries nav(database);

  const auto anonLinks = nav.backlinks("target.md", false);
  REQUIRE(anonLinks.size() == 2);
  for (const auto& d : anonLinks) REQUIRE(d.path != "secret.md");

  const auto adminLinks = nav.backlinks("target.md", true);
  REQUIRE(adminLinks.size() == 3);
}

TEST_CASE("NavQueries::backlinks re-derives from a document's CURRENT body, "
          "not a stale one, on re-save",
          "[NavQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);

  auto withBody = [](DocumentIndexEntry e, std::string body) {
    e.body = std::move(body);
    return e;
  };
  updater.upsertOne(makeEntry("target.md", "public", {}));
  updater.upsertOne(withBody(makeEntry("a.md", "public", {}), "[[target]]"));
  NavQueries nav(database);
  REQUIRE(nav.backlinks("target.md", true).size() == 1);

  // Re-save "a.md" with the link removed -- upsertOne must replace the
  // link set, not accumulate it (same discipline as replaceTagLinks).
  updater.upsertOne(withBody(makeEntry("a.md", "public", {}), "no link anymore"));
  REQUIRE(nav.backlinks("target.md", true).empty());
}
