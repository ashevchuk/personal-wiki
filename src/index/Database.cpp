#include "index/Database.h"

#include "index/schema.h"

#include <array>
#include <cstdlib>
#include <mutex>
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
constexpr std::array<const char*, 5> kMigrations = {schema::kMigration1, schema::kMigration2,
                                                      schema::kMigration3, schema::kMigration4,
                                                      schema::kMigration5};

#ifdef WIKI_ENABLE_SQLITE_VEC
// Deliberately NOT `#include <sqlite-vec.h>` here — that header pulls in
// sqlite3ext.h unless SQLITE_CORE is defined, which redefines every
// sqlite3_* symbol as a macro through a sqlite3_api diversion table this
// file never initializes (it's not itself an extension entry point) —
// that would silently break execOrThrow/sqlite3_open_v2/etc. everywhere
// else in this file. sqlite-vec.c (the loadable-extension side, where
// that diversion table IS the correct and intended pattern — see its own
// sqlite3_vec_init()) is compiled as its own separate translation unit
// (root CMakeLists.txt), so this is the ONLY declaration this file
// needs: the exact signature sqlite-vec.h itself declares, reproduced by
// hand instead of including the header that comes with side effects
// this file doesn't want.
struct sqlite3_api_routines;
extern "C" int sqlite3_vec_init(sqlite3* db, char** pzErrMsg,
                                 const sqlite3_api_routines* pApi);

// sqlite3_auto_extension() registers vec0 for every sqlite3_open() the
// process makes from here on (this Database's own connection AND
// VaultWatcher's separate one — see docs/architecture.md's "VaultWatcher
// gets its OWN Database" entry) — process-wide and permanent, so it only
// needs doing once regardless of how many Database instances get
// constructed. std::call_once (same shape as
// LocalEmbeddingProvider::ensureBackendInit()) guards that.
std::once_flag g_sqliteVecInitFlag;

void ensureSqliteVecRegistered() {
  std::call_once(g_sqliteVecInitFlag, [] {
    sqlite3_auto_extension(reinterpret_cast<void (*)()>(sqlite3_vec_init));
  });
}
#endif

}  // namespace

Database::Database(std::filesystem::path dbPath) {
#ifdef WIKI_ENABLE_SQLITE_VEC
  ensureSqliteVecRegistered();
#endif

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
