#pragma once

#include <sqlite3.h>

#include <filesystem>

namespace wikicore::index {

// Thin RAII wrapper around a single sqlite3 connection to the index db,
// plus the migration runner. This is the *only* index/db_path the process
// opens — SqliteIndex (search/documents) and, in wiki-server, the auth
// module (users/sessions) both operate on the same Database instance,
// since users/sessions/documents/FTS all live in one file per the plan's
// schema.
//
// The db is a disposable cache, never a second source of truth: if it's
// missing or its schema_version doesn't match, migrate() brings it up to
// date from nothing (an empty db is just "version 0").
class Database {
 public:
  explicit Database(std::filesystem::path dbPath);
  ~Database();

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  // Applies any migrations newer than the db's current schema_version, in
  // order, each inside its own transaction. Safe to call on an already
  // up-to-date db (no-op) or a brand-new empty file.
  void migrate();

  sqlite3* handle() const noexcept { return db_; }

  int currentSchemaVersion() const;

 private:
  sqlite3* db_ = nullptr;
};

}  // namespace wikicore::index
