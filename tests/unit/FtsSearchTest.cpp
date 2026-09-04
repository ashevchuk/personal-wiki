#include "index/Database.h"
#include "index/FtsSearch.h"
#include "index/IndexUpdater.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace fs = std::filesystem;
using namespace wikicore::index;

namespace {

class TempDb {
 public:
  TempDb()
      : path_(fs::temp_directory_path() /
              fs::path("wiki-fts-test-" +
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

DocumentIndexEntry makeEntry(std::string path, std::string title,
                              std::string visibility, std::string docType,
                              std::vector<std::string> tags, std::string body) {
  DocumentIndexEntry e;
  e.uuid = path;  // unique enough for a test fixture
  e.path = std::move(path);
  e.title = std::move(title);
  e.visibility = std::move(visibility);
  e.docType = std::move(docType);
  e.createdAt = "2026-01-01T00:00:00Z";
  e.updatedAt = "2026-01-01T00:00:00Z";
  e.tags = std::move(tags);
  e.body = std::move(body);
  e.excerpt = e.body.substr(0, 100);
  return e;
}

}  // namespace

TEST_CASE("FtsSearch text search excludes private docs unless includePrivate",
          "[FtsSearch]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);

  updater.upsertOne(makeEntry("a.md", "Public A", "public", "note", {"x"},
                               "systemd timers are great"));
  updater.upsertOne(makeEntry("b.md", "Private B", "private", "note", {"x"},
                               "systemd migration secret plan"));

  FtsSearch search(database);

  SearchQuery anonQuery;
  anonQuery.text = "systemd";
  anonQuery.includePrivate = false;
  const auto anonResults = search.search(anonQuery);
  REQUIRE(anonResults.size() == 1);
  REQUIRE(anonResults[0].path == "a.md");

  SearchQuery adminQuery = anonQuery;
  adminQuery.includePrivate = true;
  const auto adminResults = search.search(adminQuery);
  REQUIRE(adminResults.size() == 2);
}

TEST_CASE("FtsSearch highlights matches with control-byte markers, not raw "
          "document content unescaped",
          "[FtsSearch]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "A", "public", "note", {},
                               "the quick brown fox jumps"));

  FtsSearch search(database);
  SearchQuery q;
  q.text = "fox";
  q.includePrivate = true;
  const auto results = search.search(q);
  REQUIRE(results.size() == 1);
  REQUIRE(results[0].snippetIsHighlighted);
  REQUIRE(results[0].snippet.find(FtsSearch::kSnippetMatchStart) != std::string::npos);
  REQUIRE(results[0].snippet.find(FtsSearch::kSnippetMatchEnd) != std::string::npos);
  // Raw literal "<mark>" must NOT appear -- see docs/architecture.md on
  // why the markers are control bytes, not the literal tag text.
  REQUIRE(results[0].snippet.find("<mark>") == std::string::npos);
}

TEST_CASE("FtsSearch browse mode (empty text) lists by tag/type filters "
          "without requiring a MATCH query",
          "[FtsSearch]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "A", "public", "recipe", {"food"}, "pasta"));
  updater.upsertOne(makeEntry("b.md", "B", "public", "note", {"linux"}, "systemd"));

  FtsSearch search(database);
  SearchQuery q;
  q.includePrivate = true;
  q.docType = "recipe";
  const auto results = search.search(q);
  REQUIRE(results.size() == 1);
  REQUIRE(results[0].path == "a.md");
  REQUIRE_FALSE(results[0].snippetIsHighlighted);
}

TEST_CASE("FtsSearch tag filter narrows results", "[FtsSearch]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "A", "public", "note", {"food", "quick"},
                               "pasta recipe"));
  updater.upsertOne(makeEntry("b.md", "B", "public", "note", {"linux"},
                               "pasta is not mentioned here but systemd is"));

  FtsSearch search(database);
  SearchQuery q;
  q.text = "pasta";
  q.includePrivate = true;
  q.tag = "food";
  const auto results = search.search(q);
  REQUIRE(results.size() == 1);
  REQUIRE(results[0].path == "a.md");
}

TEST_CASE("FtsSearch docTypes filter uses OR semantics (a doc has one type)",
          "[FtsSearch]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "A", "public", "recipe", {}, "pasta"));
  updater.upsertOne(makeEntry("b.md", "B", "public", "note", {}, "systemd"));
  updater.upsertOne(makeEntry("c.md", "C", "public", "log", {}, "diary entry"));

  FtsSearch search(database);
  SearchQuery q;
  q.includePrivate = true;
  q.docTypes = {"recipe", "note"};
  const auto results = search.search(q);
  REQUIRE(results.size() == 2);
  for (const auto& r : results) REQUIRE(r.path != "c.md");
}

TEST_CASE("FtsSearch tags (plural) filter uses AND semantics", "[FtsSearch]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(
      makeEntry("a.md", "A", "public", "note", {"food", "quick"}, "pasta"));
  updater.upsertOne(makeEntry("b.md", "B", "public", "note", {"food"}, "soup"));

  FtsSearch search(database);
  SearchQuery q;
  q.includePrivate = true;
  q.tags = {"food", "quick"};
  const auto results = search.search(q);
  REQUIRE(results.size() == 1);
  REQUIRE(results[0].path == "a.md");
}

// Regression test for a real, live-observed report: searching "time"
// found a document containing the literal word "time" but missed one
// that only ever said "Timer"/"Timers" — porter stemming doesn't reduce
// the "-er" agent-noun suffix, so "time" and "timer" are different
// stems and an exact (non-prefix) MATCH correctly, if unhelpfully, told
// them apart. Prefix matching (FtsSearch.cpp's buildMatchExpression)
// fixes this: "time*" matches any indexed term starting with "time",
// which includes the stored stem "timer" (itself what both "Timer" and
// "Timers" stem to).
TEST_CASE("FtsSearch prefix-matches a short query against a longer stem",
          "[FtsSearch]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("timer.md", "Systemd Timers", "public", "note", {},
                               "A systemd timer unit replaces cron."));
  updater.upsertOne(makeEntry("literal.md", "Cooking", "public", "recipe", {},
                               "Add vegetables in order of cook time."));
  updater.upsertOne(makeEntry("unrelated.md", "Unrelated", "public", "note", {},
                               "Nothing about clocks here."));

  FtsSearch search(database);
  SearchQuery q;
  q.text = "time";
  q.includePrivate = true;
  const auto results = search.search(q);
  REQUIRE(results.size() == 2);
  bool foundTimer = false, foundLiteral = false;
  for (const auto& r : results) {
    if (r.path == "timer.md") foundTimer = true;
    if (r.path == "literal.md") foundLiteral = true;
  }
  REQUIRE(foundTimer);
  REQUIRE(foundLiteral);
}

// A query word that collides with FTS5's own query-language syntax used
// to go straight into MATCH unescaped — a bare "AND"/"OR"/"NOT", a
// leading '-', or an unmatched '"' could throw a MATCH syntax error
// back at the caller instead of just... searching for that word.
TEST_CASE("FtsSearch treats FTS5-syntax-colliding words as literal text, "
          "not query operators",
          "[FtsSearch]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "A", "public", "note", {},
                               "Bonnie and Clyde were partners in crime."));

  FtsSearch search(database);

  SearchQuery qAnd;
  qAnd.text = "and";
  qAnd.includePrivate = true;
  // Must not throw (a raw "AND" handed to FTS5 as an operator with
  // nothing around it is a MATCH syntax error) and must actually find
  // the literal word.
  const auto andResults = search.search(qAnd);
  REQUIRE(andResults.size() == 1);

  SearchQuery qQuote;
  qQuote.text = "crim\"e";  // embedded literal double-quote
  qQuote.includePrivate = true;
  REQUIRE_NOTHROW(search.search(qQuote));
}
