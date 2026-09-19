#include "index/EmbeddingIndexer.h"

#include "util/Time.h"

#include <sqlite3.h>

#include <stdexcept>
#include <string>

namespace wikicore::index {

namespace {

void execOrThrow(sqlite3* db, const std::string& sql) {
  char* errMsg = nullptr;
  const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
  if (rc != SQLITE_OK) {
    std::string msg = errMsg ? errMsg : "unknown sqlite error";
    sqlite3_free(errMsg);
    throw std::runtime_error("EmbeddingIndexer: sqlite error: " + msg);
  }
}

}  // namespace

EmbeddingIndexer::EmbeddingIndexer(sqlite3* db) : db_(db) {}

std::optional<std::size_t> EmbeddingIndexer::currentDimensions() const {
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_, "SELECT value FROM index_meta WHERE key = 'embedding_dimensions'",
                      -1, &stmt, nullptr);
  std::optional<std::size_t> result;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    result = static_cast<std::size_t>(std::stoul(text));
  }
  sqlite3_finalize(stmt);
  return result;
}

namespace {

std::optional<std::string> readMeta(sqlite3* db, const char* key) {
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db, "SELECT value FROM index_meta WHERE key = ?", -1, &stmt, nullptr);
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  std::optional<std::string> result;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    result = text ? std::string(text) : std::string();
  }
  sqlite3_finalize(stmt);
  return result;
}

}  // namespace

void EmbeddingIndexer::ensureTable(std::size_t dimensions, const std::string& modelIdentifier) {
  const auto existingDims = currentDimensions();
  const auto existingModel = readMeta(db_, "embedding_model_id");
  if (existingDims.has_value() && *existingDims == dimensions && existingModel.has_value() &&
      *existingModel == modelIdentifier) {
    return;  // already at the right width, from the right model — nothing to do
  }

  execOrThrow(db_, "BEGIN IMMEDIATE;");
  try {
    // DROP is a no-op the first time (table doesn't exist yet); on a
    // dimension OR model change it deliberately discards every stored
    // vector — see the class comment on why that's accepted, not worked
    // around. document_embedding_state is cleared alongside it: a
    // document whose text didn't change still needs a real re-embed
    // against the new model, so its content-hash tracking can't be left
    // pointing at a hash that would now wrongly look "still current".
    execOrThrow(db_, "DROP TABLE IF EXISTS document_embeddings;");
    execOrThrow(db_, "DELETE FROM document_embedding_state;");
    execOrThrow(db_, "CREATE VIRTUAL TABLE document_embeddings USING vec0("
                      "document_rowid INTEGER PRIMARY KEY, "
                      "embedding float[" +
                          std::to_string(dimensions) +
                          "] distance_metric=cosine"
                          ");");
    execOrThrow(db_,
                "INSERT INTO index_meta(key, value) VALUES ('embedding_dimensions', '" +
                    std::to_string(dimensions) +
                    "') ON CONFLICT(key) DO UPDATE SET value = excluded.value;");

    sqlite3_stmt* modelStmt = nullptr;
    sqlite3_prepare_v2(db_,
                        "INSERT INTO index_meta(key, value) VALUES ('embedding_model_id', ?) "
                        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;",
                        -1, &modelStmt, nullptr);
    sqlite3_bind_text(modelStmt, 1, modelIdentifier.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(modelStmt);
    sqlite3_finalize(modelStmt);
    if (rc != SQLITE_DONE) {
      throw std::runtime_error(std::string("EmbeddingIndexer::ensureTable: ") +
                                sqlite3_errmsg(db_));
    }

    execOrThrow(db_, "COMMIT;");
  } catch (...) {
    execOrThrow(db_, "ROLLBACK;");
    throw;
  }
}

void EmbeddingIndexer::upsertOne(sqlite3_int64 documentRowId,
                                  const std::vector<float>& embedding) {
  const auto dims = currentDimensions();
  if (!dims.has_value()) {
    throw std::runtime_error(
        "EmbeddingIndexer::upsertOne: document_embeddings doesn't exist yet — call "
        "ensureTable() first");
  }
  if (embedding.size() != *dims) {
    throw std::runtime_error(
        "EmbeddingIndexer::upsertOne: embedding has " + std::to_string(embedding.size()) +
        " dimensions, table expects " + std::to_string(*dims));
  }

  // vec0 tables don't support ON CONFLICT/UPSERT (confirmed against
  // sqlite-vec's own docs and test suite) — delete-then-insert inside one
  // transaction gives the same "upsert" observable effect.
  execOrThrow(db_, "BEGIN IMMEDIATE;");
  try {
    sqlite3_stmt* del = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM document_embeddings WHERE document_rowid = ?", -1,
                        &del, nullptr);
    sqlite3_bind_int64(del, 1, documentRowId);
    sqlite3_step(del);
    sqlite3_finalize(del);

    sqlite3_stmt* ins = nullptr;
    sqlite3_prepare_v2(db_, "INSERT INTO document_embeddings(document_rowid, embedding) "
                            "VALUES (?, ?)",
                        -1, &ins, nullptr);
    sqlite3_bind_int64(ins, 1, documentRowId);
    sqlite3_bind_blob(ins, 2, embedding.data(),
                       static_cast<int>(embedding.size() * sizeof(float)), SQLITE_TRANSIENT);
    const int rc = sqlite3_step(ins);
    sqlite3_finalize(ins);
    if (rc != SQLITE_DONE) {
      throw std::runtime_error(std::string("EmbeddingIndexer::upsertOne: insert failed: ") +
                                sqlite3_errmsg(db_));
    }

    execOrThrow(db_, "COMMIT;");
  } catch (...) {
    execOrThrow(db_, "ROLLBACK;");
    throw;
  }
}

void EmbeddingIndexer::removeOne(sqlite3_int64 documentRowId) {
  if (!currentDimensions().has_value()) {
    return;  // table doesn't exist — matches IndexUpdater's own no-op-if-absent shape
  }
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_, "DELETE FROM document_embeddings WHERE document_rowid = ?", -1,
                      &stmt, nullptr);
  sqlite3_bind_int64(stmt, 1, documentRowId);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

std::vector<EmbeddingIndexer::Neighbor> EmbeddingIndexer::nearest(
    const std::vector<float>& queryEmbedding, int limit) const {
  const auto dims = currentDimensions();
  if (!dims.has_value()) {
    return {};  // no table yet — no neighbors, not an error
  }
  if (queryEmbedding.size() != *dims) {
    throw std::runtime_error(
        "EmbeddingIndexer::nearest: query embedding has " +
        std::to_string(queryEmbedding.size()) + " dimensions, table expects " +
        std::to_string(*dims));
  }

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_,
                      "SELECT document_rowid, distance FROM document_embeddings "
                      "WHERE embedding MATCH ?1 ORDER BY distance LIMIT ?2",
                      -1, &stmt, nullptr);
  sqlite3_bind_blob(stmt, 1, queryEmbedding.data(),
                     static_cast<int>(queryEmbedding.size() * sizeof(float)), SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, limit);

  std::vector<Neighbor> results;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    results.push_back(
        {sqlite3_column_int64(stmt, 0), sqlite3_column_double(stmt, 1)});
  }
  sqlite3_finalize(stmt);
  return results;
}

bool EmbeddingIndexer::needsEmbedding(sqlite3_int64 documentRowId,
                                       const std::string& currentContentHash) const {
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(
      db_, "SELECT content_hash FROM document_embedding_state WHERE document_rowid = ?", -1,
      &stmt, nullptr);
  sqlite3_bind_int64(stmt, 1, documentRowId);
  bool needs = true;  // no row at all -> never attempted -> definitely needs it
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const std::string storedHash = text ? std::string(text) : std::string();
    needs = storedHash != currentContentHash;
  }
  sqlite3_finalize(stmt);
  return needs;
}

void EmbeddingIndexer::recordEmbeddingSuccess(sqlite3_int64 documentRowId,
                                               const std::string& contentHash) {
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_,
                      "INSERT INTO document_embedding_state"
                      "(document_rowid, content_hash, last_error, updated_at) "
                      "VALUES (?1, ?2, NULL, ?3) "
                      "ON CONFLICT(document_rowid) DO UPDATE SET "
                      "content_hash = excluded.content_hash, last_error = NULL, "
                      "updated_at = excluded.updated_at;",
                      -1, &stmt, nullptr);
  sqlite3_bind_int64(stmt, 1, documentRowId);
  sqlite3_bind_text(stmt, 2, contentHash.c_str(), -1, SQLITE_TRANSIENT);
  const std::string now = util::nowIso8601();
  sqlite3_bind_text(stmt, 3, now.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw std::runtime_error(std::string("EmbeddingIndexer::recordEmbeddingSuccess: ") +
                              sqlite3_errmsg(db_));
  }
}

void EmbeddingIndexer::recordEmbeddingFailure(sqlite3_int64 documentRowId,
                                               const std::string& errorMessage) {
  // content_hash deliberately NOT touched on the UPDATE branch — see this
  // method's own doc comment in EmbeddingIndexer.h for why leaving it
  // stale is what makes the next rescan retry automatically. The INSERT
  // branch (first-ever attempt, no success yet) uses '' — the same
  // "never a real hash" sentinel the column's own DEFAULT '' establishes
  // in schema.h — so it's guaranteed to mismatch any real content hash.
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_,
                      "INSERT INTO document_embedding_state"
                      "(document_rowid, content_hash, last_error, updated_at) "
                      "VALUES (?1, '', ?2, ?3) "
                      "ON CONFLICT(document_rowid) DO UPDATE SET "
                      "last_error = excluded.last_error, updated_at = excluded.updated_at;",
                      -1, &stmt, nullptr);
  sqlite3_bind_int64(stmt, 1, documentRowId);
  sqlite3_bind_text(stmt, 2, errorMessage.c_str(), -1, SQLITE_TRANSIENT);
  const std::string now = util::nowIso8601();
  sqlite3_bind_text(stmt, 3, now.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw std::runtime_error(std::string("EmbeddingIndexer::recordEmbeddingFailure: ") +
                              sqlite3_errmsg(db_));
  }
}

void EmbeddingIndexer::clearEmbeddingState(sqlite3_int64 documentRowId) {
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_, "DELETE FROM document_embedding_state WHERE document_rowid = ?", -1,
                      &stmt, nullptr);
  sqlite3_bind_int64(stmt, 1, documentRowId);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

std::vector<EmbeddingIndexer::PendingDocument> EmbeddingIndexer::listNeedingAttention() const {
  // LEFT JOIN: a document with NO document_embedding_state row at all
  // (des.document_rowid IS NULL) has never been attempted; one that DOES
  // have a row but des.last_error IS NOT NULL had its most recent
  // attempt fail. Either way it needs a human's attention, not just a
  // routine re-embed on next save.
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db_,
                      "SELECT d.rowid_id, d.path, d.title, des.last_error "
                      "FROM documents d "
                      "LEFT JOIN document_embedding_state des ON des.document_rowid = d.rowid_id "
                      "WHERE des.document_rowid IS NULL OR des.last_error IS NOT NULL "
                      "ORDER BY d.path;",
                      -1, &stmt, nullptr);
  std::vector<PendingDocument> results;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    PendingDocument doc;
    doc.documentRowId = sqlite3_column_int64(stmt, 0);
    const auto* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    doc.path = path ? path : "";
    const auto* title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    doc.title = title ? title : "";
    if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
      const auto* err = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
      doc.lastError = err ? std::string(err) : std::string();
    }
    results.push_back(std::move(doc));
  }
  sqlite3_finalize(stmt);
  return results;
}

}  // namespace wikicore::index
