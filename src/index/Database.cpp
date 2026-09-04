#include "index/Database.h"

#include "index/schema.h"

#include <array>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace wikicore::index {

namespace {

void execOrThrow(sqlite3* db, const char* sql) {
  char* errMsg = nullptr;
  const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
  if (rc != SQLITE_OK) {
    std::string msg = errMsg ? errMsg : "unknown sqlite error";
    sqlite3_free(errMsg);
    throw std::runtime_error("sqlite error: " + msg);
  }
}

// Ordered list of migrations; index 0 is schema_version 1, etc. Add new
// entries at the end only — never edit or reorder an already-shipped one.
constexpr std::array<const char*, 2> kMigrations = {schema::kMigration1, schema::kMigration2};

}  // namespace

Database::Database(std::filesystem::path dbPath) {
  // Parent directory must exist before sqlite3 will create the db file.
  if (dbPath.has_parent_path()) {
    std::filesystem::create_directories(dbPath.parent_path());
  }

  const int rc =
      sqlite3_open_v2(dbPath.string().c_str(), &db_,
                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  if (rc != SQLITE_OK) {
    const std::string msg = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
    throw std::runtime_error("failed to open database '" + dbPath.string() +
                              "': " + msg);
  }

  // WAL + foreign_keys are session pragmas (not persisted in the schema),
  // so they're set on every open, not just at migration time.
  execOrThrow(db_, "PRAGMA journal_mode = WAL;");
  execOrThrow(db_, "PRAGMA foreign_keys = ON;");
  execOrThrow(db_, "PRAGMA busy_timeout = 5000;");
}

Database::~Database() {
  if (db_) sqlite3_close(db_);
}

int Database::currentSchemaVersion() const {
  // index_meta doesn't exist yet on a brand-new db — that's version 0.
  sqlite3_stmt* checkTable = nullptr;
  sqlite3_prepare_v2(
      db_,
      "SELECT 1 FROM sqlite_master WHERE type='table' AND name='index_meta'",
      -1, &checkTable, nullptr);
  const bool tableExists = sqlite3_step(checkTable) == SQLITE_ROW;
  sqlite3_finalize(checkTable);
  if (!tableExists) return 0;

  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(
      db_, "SELECT value FROM index_meta WHERE key = 'schema_version'", -1,
      &stmt, nullptr);
  int version = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    version = std::atoi(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
  }
  sqlite3_finalize(stmt);
  return version;
}

void Database::migrate() {
  int version = currentSchemaVersion();
  const int target = static_cast<int>(kMigrations.size());

  for (int v = version; v < target; ++v) {
    execOrThrow(db_, "BEGIN IMMEDIATE;");
    try {
      execOrThrow(db_, kMigrations[static_cast<size_t>(v)]);

      const std::string upsert =
          "INSERT INTO index_meta(key, value) VALUES ('schema_version', '" +
          std::to_string(v + 1) +
          "') ON CONFLICT(key) DO UPDATE SET value = excluded.value;";
      execOrThrow(db_, upsert.c_str());

      execOrThrow(db_, "COMMIT;");
    } catch (...) {
      execOrThrow(db_, "ROLLBACK;");
      throw;
    }
  }
}

}  // namespace wikicore::index
