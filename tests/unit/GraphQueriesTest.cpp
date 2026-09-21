#include "index/Database.h"
#include "index/GraphQueries.h"
#include "index/IndexUpdater.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>

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

bool hasNode(const std::vector<GraphNode>& nodes, const std::string& path) {
  for (const auto& n : nodes) {
    if (n.path == path) return true;
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

TEST_CASE("GraphQueries::around returns nullopt for a missing document",
          "[GraphQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "public"));

  GraphQueries graph(database);
  REQUIRE_FALSE(graph.around("missing.md", true).has_value());
  REQUIRE_FALSE(graph.around("missing.md", false).has_value());
}

TEST_CASE("GraphQueries::around returns nullopt for a private center to an "
          "anonymous caller, the same as a miss",
          "[GraphQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("priv.md", "private", "See [[pub.md]]."));
  updater.upsertOne(makeEntry("pub.md", "public"));

  GraphQueries graph(database);
  REQUIRE_FALSE(graph.around("priv.md", false).has_value());

  auto admin = graph.around("priv.md", true);
  REQUIRE(admin.has_value());
  REQUIRE(hasNode(admin->nodes, "priv.md"));
  REQUIRE(hasNode(admin->nodes, "pub.md"));
  REQUIRE(hasEdge(admin->edges, "priv.md", "pub.md"));
}

TEST_CASE("GraphQueries::around never leaks a private neighbor to an "
          "anonymous caller, from either end of the edge",
          "[GraphQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("pub-source.md", "public", "See [[priv-target]]."));
  updater.upsertOne(makeEntry("priv-target.md", "private"));
  updater.upsertOne(makeEntry("priv-source.md", "private", "See [[pub-target]]."));
  updater.upsertOne(makeEntry("pub-target.md", "public"));

  GraphQueries graph(database);

  auto fromPub = graph.around("pub-source.md", false);
  REQUIRE(fromPub.has_value());
  REQUIRE(fromPub->nodes.size() == 1);
  REQUIRE(hasNode(fromPub->nodes, "pub-source.md"));
  REQUIRE_FALSE(hasNode(fromPub->nodes, "priv-target.md"));
  REQUIRE(fromPub->edges.empty());

  auto fromPubAdmin = graph.around("pub-source.md", true);
  REQUIRE(fromPubAdmin.has_value());
  REQUIRE(hasNode(fromPubAdmin->nodes, "priv-target.md"));
  REQUIRE(hasEdge(fromPubAdmin->edges, "pub-source.md", "priv-target.md"));
}

TEST_CASE("GraphQueries::around walks the connected component, not just 1-hop",
          "[GraphQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "public", "See [[b]]."));
  updater.upsertOne(makeEntry("b.md", "public", "See [[c]]."));
  updater.upsertOne(makeEntry("c.md", "public"));
  updater.upsertOne(makeEntry("unrelated.md", "public"));

  GraphQueries graph(database);
  auto nb = graph.around("a.md", true);
  REQUIRE(nb.has_value());
  REQUIRE(nb->nodes.size() == 3);
  REQUIRE(hasNode(nb->nodes, "a.md"));
  REQUIRE(hasNode(nb->nodes, "b.md"));
  REQUIRE(hasNode(nb->nodes, "c.md"));
  REQUIRE_FALSE(hasNode(nb->nodes, "unrelated.md"));
  REQUIRE(hasEdge(nb->edges, "a.md", "b.md"));
  REQUIRE(hasEdge(nb->edges, "b.md", "c.md"));
}

TEST_CASE("GraphQueries::around includes an induced edge that does not touch "
          "the center",
          "[GraphQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "public", "See [[b]] and [[c]]."));
  updater.upsertOne(makeEntry("b.md", "public", "See [[c]]."));
  updater.upsertOne(makeEntry("c.md", "public"));

  GraphQueries graph(database);
  auto nb = graph.around("a.md", true);
  REQUIRE(nb.has_value());
  REQUIRE(nb->nodes.size() == 3);
  REQUIRE(hasEdge(nb->edges, "a.md", "b.md"));
  REQUIRE(hasEdge(nb->edges, "a.md", "c.md"));
  REQUIRE(hasEdge(nb->edges, "b.md", "c.md"));
}

TEST_CASE("GraphQueries::around does not walk through a private document to "
          "reach a further public one for an anonymous caller",
          "[GraphQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "public", "See [[b]]."));
  updater.upsertOne(makeEntry("b.md", "private", "See [[c]]."));
  updater.upsertOne(makeEntry("c.md", "public"));

  GraphQueries graph(database);
  auto anon = graph.around("a.md", false);
  REQUIRE(anon.has_value());
  REQUIRE(anon->nodes.size() == 1);
  REQUIRE(hasNode(anon->nodes, "a.md"));
  REQUIRE_FALSE(hasNode(anon->nodes, "b.md"));
  REQUIRE_FALSE(hasNode(anon->nodes, "c.md"));
  REQUIRE(anon->edges.empty());

  auto admin = graph.around("a.md", true);
  REQUIRE(admin.has_value());
  REQUIRE(admin->nodes.size() == 3);
  REQUIRE(hasNode(admin->nodes, "c.md"));
  REQUIRE(hasEdge(admin->edges, "a.md", "b.md"));
  REQUIRE(hasEdge(admin->edges, "b.md", "c.md"));
}

TEST_CASE("GraphQueries::around of a cycle terminates and includes every node",
          "[GraphQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "public", "See [[b]]."));
  updater.upsertOne(makeEntry("b.md", "public", "See [[c]]."));
  updater.upsertOne(makeEntry("c.md", "public", "See [[a]]."));

  GraphQueries graph(database);
  auto nb = graph.around("a.md", true);
  REQUIRE(nb.has_value());
  REQUIRE(nb->nodes.size() == 3);
  REQUIRE(hasEdge(nb->edges, "a.md", "b.md"));
  REQUIRE(hasEdge(nb->edges, "b.md", "c.md"));
  REQUIRE(hasEdge(nb->edges, "c.md", "a.md"));
}

TEST_CASE("GraphQueries::around of an isolated visible document is just itself",
          "[GraphQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("alone.md", "public"));

  GraphQueries graph(database);
  auto nb = graph.around("alone.md", false);
  REQUIRE(nb.has_value());
  REQUIRE(nb->nodes.size() == 1);
  REQUIRE(hasNode(nb->nodes, "alone.md"));
  REQUIRE(nb->edges.empty());
}

TEST_CASE("GraphQueries::around treats a LIKE-wildcard path as a literal miss, "
          "not a pattern",
          "[GraphQueries]") {
  TempDb db;
  Database database(db.path());
  database.migrate();
  IndexUpdater updater(database);
  updater.upsertOne(makeEntry("a.md", "public"));

  GraphQueries graph(database);
  REQUIRE_FALSE(graph.around("%", true).has_value());
  REQUIRE_FALSE(graph.around("_.md", true).has_value());
}
