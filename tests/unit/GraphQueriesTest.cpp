#include "index/Database.h"
#include "index/GraphQueries.h"
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
              fs::path("wiki-graph-test-" +
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
                              std::string body = "") {
  DocumentIndexEntry e;
  e.uuid = path;
  e.path = std::move(path);
  e.title = e.path;
  e.visibility = std::move(visibility);
  e.createdAt = e.updatedAt = "2026-01-01T00:00:00Z";
  e.body = std::move(body);
  return e;
}

bool hasEdge(const std::vector<GraphEdge>& edges, const std::string& source,
             const std::string& target) {
  for (const auto& e : edges) {
    if (e.source == source && e.target == target) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("GraphQueries::nodes lists visible documents, visibility-gated",
          "[GraphQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "public"));
  updater.upsertOne(makeEntry("b.md", "private"));

  GraphQueries graph(database);
  REQUIRE(graph.nodes(false).size() == 1);
  REQUIRE(graph.nodes(true).size() == 2);
}

TEST_CASE("GraphQueries::edges finds [[wiki-link]] edges between real documents",
          "[GraphQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "public", "See [[b]]."));
  updater.upsertOne(makeEntry("b.md", "public"));

  GraphQueries graph(database);
  auto edges = graph.edges(true);
  REQUIRE(edges.size() == 1);
  REQUIRE(hasEdge(edges, "a.md", "b.md"));
}

TEST_CASE("GraphQueries::edges excludes a red link (target document doesn't exist)",
          "[GraphQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "public", "See [[nonexistent]]."));

  GraphQueries graph(database);
  REQUIRE(graph.edges(true).empty());
}

TEST_CASE("GraphQueries::edges never leaks an edge touching a private document "
          "to an anonymous caller, from either end",
          "[GraphQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  // public -> private (source visible, target not)
  updater.upsertOne(makeEntry("pub-source.md", "public", "See [[priv-target]]."));
  updater.upsertOne(makeEntry("priv-target.md", "private"));
  // private -> public (source not visible, target is)
  updater.upsertOne(makeEntry("priv-source.md", "private", "See [[pub-target]]."));
  updater.upsertOne(makeEntry("pub-target.md", "public"));

  GraphQueries graph(database);
  auto anonEdges = graph.edges(false);
  REQUIRE(anonEdges.empty());

  auto adminEdges = graph.edges(true);
  REQUIRE(adminEdges.size() == 2);
  REQUIRE(hasEdge(adminEdges, "pub-source.md", "priv-target.md"));
  REQUIRE(hasEdge(adminEdges, "priv-source.md", "pub-target.md"));
}

TEST_CASE("GraphQueries::edges re-derives from a document's CURRENT body on "
          "re-save, same as NavQueries::backlinks",
          "[GraphQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "public", "[[b]]"));
  updater.upsertOne(makeEntry("b.md", "public"));

  GraphQueries graph(database);
  REQUIRE(graph.edges(true).size() == 1);

  updater.upsertOne(makeEntry("a.md", "public", "no link anymore"));
  REQUIRE(graph.edges(true).empty());
}
