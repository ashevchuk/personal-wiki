#include "index/Database.h"
#include "index/EmbeddingIndexer.h"
#include "index/Statement.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace fs = std::filesystem;
using namespace wikicore::index;

namespace {

class TempDb {
 public:
  TempDb()
      : path_(fs::temp_directory_path() /
              fs::path("wiki-embeddingindexer-test-" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".db")) {
    fs::remove(path_);
    db_ = std::make_unique<Database>(path_);
    db_->migrate();
  }
  ~TempDb() { fs::remove(path_); }
  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;

  Database& db() { return *db_; }

 private:
  fs::path path_;
  std::unique_ptr<Database> db_;
};

std::vector<float> vec3(float a, float b, float c) { return {a, b, c}; }

// document_embedding_state has a real FOREIGN KEY on documents(rowid_id)
// (ON DELETE CASCADE — see schema.h's kMigration5) — unlike
// document_embeddings itself (a vec0 virtual table, which doesn't support
// REFERENCES constraints), so any test touching
// needsEmbedding()/recordEmbeddingSuccess()/recordEmbeddingFailure() needs
// a REAL documents row to point at, not an arbitrary rowid.
int64_t insertDummyDocument(Database& db, const std::string& path) {
  Statement stmt(db.handle(), "INSERT INTO documents(uuid, path, title, visibility, "
                               "created_at, updated_at, file_mtime, file_size) "
                               "VALUES (?1, ?1, ?1, 'public', '2026-01-01T00:00:00Z', "
                               "'2026-01-01T00:00:00Z', 0, 0);");
  stmt.bind(1, path);
  stmt.run();
  return stmt.lastInsertRowId();
}

}  // namespace

TEST_CASE("EmbeddingIndexer: currentDimensions() is nullopt before ensureTable()",
          "[EmbeddingIndexer]") {
  TempDb env;
  EmbeddingIndexer indexer(env.db().handle());
  REQUIRE_FALSE(indexer.currentDimensions().has_value());
}

TEST_CASE("EmbeddingIndexer: ensureTable() creates the table at the requested width",
          "[EmbeddingIndexer]") {
  TempDb env;
  EmbeddingIndexer indexer(env.db().handle());
  indexer.ensureTable(3, "test-model-a");
  REQUIRE(indexer.currentDimensions() == 3);
}

TEST_CASE("EmbeddingIndexer: calling ensureTable() again with the SAME width is a "
          "no-op — existing rows survive",
          "[EmbeddingIndexer]") {
  TempDb env;
  EmbeddingIndexer indexer(env.db().handle());
  indexer.ensureTable(3, "test-model-a");
  indexer.upsertOne(1, vec3(1.0f, 0.0f, 0.0f));

  indexer.ensureTable(3, "test-model-a");  // same width again

  const auto results = indexer.nearest(vec3(1.0f, 0.0f, 0.0f), 5);
  REQUIRE(results.size() == 1);
  REQUIRE(results[0].documentRowId == 1);
}

TEST_CASE("EmbeddingIndexer: calling ensureTable() with a DIFFERENT width drops and "
          "recreates — a provider/dimension swap loses existing vectors, by design",
          "[EmbeddingIndexer]") {
  TempDb env;
  EmbeddingIndexer indexer(env.db().handle());
  indexer.ensureTable(3, "test-model-a");
  indexer.upsertOne(1, vec3(1.0f, 0.0f, 0.0f));

  indexer.ensureTable(4, "test-model-a");  // different width — table recreated, row 1 gone

  REQUIRE(indexer.currentDimensions() == 4);
  const auto results = indexer.nearest({1.0f, 0.0f, 0.0f, 0.0f}, 5);
  REQUIRE(results.empty());
}

TEST_CASE("EmbeddingIndexer: upsertOne() rejects an embedding of the wrong width",
          "[EmbeddingIndexer]") {
  TempDb env;
  EmbeddingIndexer indexer(env.db().handle());
  indexer.ensureTable(3, "test-model-a");
  REQUIRE_THROWS_AS(indexer.upsertOne(1, {1.0f, 0.0f}), std::runtime_error);
}

TEST_CASE("EmbeddingIndexer: upsertOne() on an existing rowid replaces, not "
          "duplicates, its vector",
          "[EmbeddingIndexer]") {
  TempDb env;
  EmbeddingIndexer indexer(env.db().handle());
  indexer.ensureTable(3, "test-model-a");
  indexer.upsertOne(1, vec3(1.0f, 0.0f, 0.0f));
  indexer.upsertOne(1, vec3(0.0f, 1.0f, 0.0f));  // replace

  const auto results = indexer.nearest(vec3(0.0f, 1.0f, 0.0f), 10);
  REQUIRE(results.size() == 1);  // still just one row for rowid 1, not two
  REQUIRE(results[0].documentRowId == 1);
}

TEST_CASE("EmbeddingIndexer: nearest() ranks closer vectors first (real cosine "
          "ranking, not insertion order)",
          "[EmbeddingIndexer]") {
  TempDb env;
  EmbeddingIndexer indexer(env.db().handle());
  indexer.ensureTable(3, "test-model-a");

  // Inserted in an order that would be WRONG if nearest() just returned
  // rows in insertion/rowid order instead of actually ranking by distance.
  indexer.upsertOne(1, vec3(0.0f, 0.0f, 1.0f));  // orthogonal to the query
  indexer.upsertOne(2, vec3(1.0f, 0.0f, 0.0f));  // identical to the query
  indexer.upsertOne(3, vec3(0.9f, 0.1f, 0.0f));  // close to the query

  const auto results = indexer.nearest(vec3(1.0f, 0.0f, 0.0f), 3);
  REQUIRE(results.size() == 3);
  REQUIRE(results[0].documentRowId == 2);  // identical — closest
  REQUIRE(results[1].documentRowId == 3);  // close second
  REQUIRE(results[2].documentRowId == 1);  // orthogonal — farthest
  REQUIRE(results[0].distance < results[1].distance);
  REQUIRE(results[1].distance < results[2].distance);
}

TEST_CASE("EmbeddingIndexer: removeOne() removes exactly that row, leaves others",
          "[EmbeddingIndexer]") {
  TempDb env;
  EmbeddingIndexer indexer(env.db().handle());
  indexer.ensureTable(3, "test-model-a");
  indexer.upsertOne(1, vec3(1.0f, 0.0f, 0.0f));
  indexer.upsertOne(2, vec3(0.0f, 1.0f, 0.0f));

  indexer.removeOne(1);

  const auto results = indexer.nearest(vec3(1.0f, 0.0f, 0.0f), 10);
  REQUIRE(results.size() == 1);
  REQUIRE(results[0].documentRowId == 2);
}

TEST_CASE("EmbeddingIndexer: removeOne() on a nonexistent rowid is a no-op, not an "
          "error",
          "[EmbeddingIndexer]") {
  TempDb env;
  EmbeddingIndexer indexer(env.db().handle());
  indexer.ensureTable(3, "test-model-a");
  REQUIRE_NOTHROW(indexer.removeOne(999));
}

TEST_CASE("EmbeddingIndexer: nearest() on a table that doesn't exist yet returns "
          "empty, not an error",
          "[EmbeddingIndexer]") {
  TempDb env;
  EmbeddingIndexer indexer(env.db().handle());
  REQUIRE(indexer.nearest(vec3(1.0f, 0.0f, 0.0f), 5).empty());
}

TEST_CASE("EmbeddingIndexer: ensureTable() with the SAME dimensions but a "
          "DIFFERENT model identifier drops and recreates — two models can "
          "share a dimension count while being genuinely incompatible vector "
          "spaces",
          "[EmbeddingIndexer]") {
  TempDb env;
  EmbeddingIndexer indexer(env.db().handle());
  indexer.ensureTable(3, "test-model-a");
  indexer.upsertOne(1, vec3(1.0f, 0.0f, 0.0f));

  indexer.ensureTable(3, "test-model-b");  // same width, DIFFERENT model

  REQUIRE(indexer.currentDimensions() == 3);  // still 3 — width alone didn't change
  const auto results = indexer.nearest(vec3(1.0f, 0.0f, 0.0f), 5);
  REQUIRE(results.empty());  // but the old vector is gone — model swap, not a no-op
}

TEST_CASE("EmbeddingIndexer: needsEmbedding() is true for a rowid with no "
          "tracking row at all — never attempted",
          "[EmbeddingIndexer]") {
  TempDb env;
  EmbeddingIndexer indexer(env.db().handle());
  indexer.ensureTable(3, "test-model-a");
  REQUIRE(indexer.needsEmbedding(1, "some-hash"));
}

TEST_CASE("EmbeddingIndexer: recordEmbeddingSuccess() makes needsEmbedding() "
          "false for the SAME hash, true for a DIFFERENT one",
          "[EmbeddingIndexer]") {
  TempDb env;
  EmbeddingIndexer indexer(env.db().handle());
  indexer.ensureTable(3, "test-model-a");
  const int64_t rowId = insertDummyDocument(env.db(), "a.md");
  indexer.recordEmbeddingSuccess(rowId, "hash-v1");

  REQUIRE_FALSE(indexer.needsEmbedding(rowId, "hash-v1"));  // unchanged content
  REQUIRE(indexer.needsEmbedding(rowId, "hash-v2"));         // content changed
}

TEST_CASE("EmbeddingIndexer: recordEmbeddingFailure() leaves needsEmbedding() "
          "true — a failed attempt must self-heal on the next try, not be "
          "mistaken for success",
          "[EmbeddingIndexer]") {
  TempDb env;
  EmbeddingIndexer indexer(env.db().handle());
  indexer.ensureTable(3, "test-model-a");
  const int64_t rowId = insertDummyDocument(env.db(), "a.md");
  indexer.recordEmbeddingFailure(rowId, "network timeout");
  REQUIRE(indexer.needsEmbedding(rowId, "hash-v1"));
}

TEST_CASE("EmbeddingIndexer: recordEmbeddingFailure() AFTER a prior success "
          "does not touch the old success hash — the vector stored for "
          "hash-v1 is still real and correct, only the ATTEMPTED hash-v2 "
          "needs a retry",
          "[EmbeddingIndexer]") {
  TempDb env;
  EmbeddingIndexer indexer(env.db().handle());
  indexer.ensureTable(3, "test-model-a");
  const int64_t rowId = insertDummyDocument(env.db(), "a.md");
  indexer.recordEmbeddingSuccess(rowId, "hash-v1");
  indexer.recordEmbeddingFailure(rowId, "network timeout");  // content changed to v2, embed failed

  REQUIRE(indexer.needsEmbedding(rowId, "hash-v2"));  // still needs it — failure didn't fix anything
  // The originally tried expectation here — "even hash-v1 counts as stale
  // now" — turned out to be wrong when this test was first run: the
  // vector stored under hash-v1 is untouched by the failure (upsertOne()
  // for v2 never ran), so it's genuinely still correct for that exact
  // content. A real, useful finding caught by writing the test, not
  // something to paper over with a looser assertion.
  REQUIRE_FALSE(indexer.needsEmbedding(rowId, "hash-v1"));
}

TEST_CASE("EmbeddingIndexer: listNeedingAttention() lists never-attempted and "
          "failed documents, excludes successfully-embedded ones",
          "[EmbeddingIndexer]") {
  TempDb env;
  Database& db = env.db();
  EmbeddingIndexer indexer(db.handle());
  indexer.ensureTable(3, "test-model-a");

  // Insert three real documents via the normal path so listNeedingAttention()'s
  // JOIN against `documents` has real rows to match against.
  auto insertOne = [&](const std::string& uuid, const std::string& path) {
    Statement stmt(db.handle(), "INSERT INTO documents(uuid, path, title, visibility, "
                                 "created_at, updated_at, file_mtime, file_size) "
                                 "VALUES (?1, ?2, ?2, 'public', '2026-01-01T00:00:00Z', "
                                 "'2026-01-01T00:00:00Z', 0, 0);");
    stmt.bind(1, uuid).bind(2, path);
    stmt.run();
    return stmt.lastInsertRowId();
  };
  const int64_t okRowId = insertOne("u1", "ok.md");
  const int64_t failedRowId = insertOne("u2", "failed.md");
  const int64_t neverTriedRowId = insertOne("u3", "never-tried.md");
  (void)neverTriedRowId;

  indexer.recordEmbeddingSuccess(okRowId, "hash-ok");
  indexer.recordEmbeddingFailure(failedRowId, "rate limited");
  // never-tried.md gets no tracking row at all — deliberately.

  const auto pending = indexer.listNeedingAttention();
  REQUIRE(pending.size() == 2);

  bool sawFailed = false, sawNeverTried = false;
  for (const auto& doc : pending) {
    if (doc.path == "failed.md") {
      sawFailed = true;
      REQUIRE(doc.lastError == "rate limited");
    }
    if (doc.path == "never-tried.md") {
      sawNeverTried = true;
      REQUIRE_FALSE(doc.lastError.has_value());
    }
    REQUIRE(doc.path != "ok.md");  // successfully-embedded doc must NOT appear
  }
  REQUIRE(sawFailed);
  REQUIRE(sawNeverTried);
}
