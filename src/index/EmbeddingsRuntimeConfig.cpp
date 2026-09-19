#include "index/EmbeddingsRuntimeConfig.h"

#include <sqlite3.h>

#include <stdexcept>
#include <string>

namespace wikicore::index {

namespace {

sqlite3_stmt* prepare(sqlite3* db, const char* sql) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error(std::string("prepare failed: ") + sqlite3_errmsg(db));
  }
  return stmt;
}

void run(sqlite3* db, sqlite3_stmt* stmt) {
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw std::runtime_error(std::string("statement failed: ") + sqlite3_errmsg(db));
  }
}

}  // namespace

bool EmbeddingsRuntimeConfig::isVectorSearchEnabled() const {
  sqlite3_stmt* stmt = prepare(
      db_.handle(), "SELECT vector_search_enabled FROM embeddings_runtime_config WHERE id = 1;");
  bool enabled = true;  // no row yet == default-on, matches schema's own DEFAULT 1
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    enabled = sqlite3_column_int64(stmt, 0) != 0;
  }
  sqlite3_finalize(stmt);
  return enabled;
}

void EmbeddingsRuntimeConfig::setVectorSearchEnabled(bool enabled) {
  sqlite3_stmt* stmt = prepare(
      db_.handle(),
      "INSERT INTO embeddings_runtime_config(id, vector_search_enabled) VALUES (1, ?1) "
      "ON CONFLICT(id) DO UPDATE SET vector_search_enabled = excluded.vector_search_enabled;");
  sqlite3_bind_int64(stmt, 1, enabled ? 1 : 0);
  run(db_.handle(), stmt);
}

}  // namespace wikicore::index
