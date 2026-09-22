#pragma once

#include <sqlite3.h>

#include <filesystem>
#include <string>
#include <vector>

namespace wikicore::index {

// Result of Database::ensureUsable(). rebuilt=false is the common path
// (file already healthy). When rebuilt=true the previous file was
// quarantined next to the live path; admin/MCP rows are copied across
// when those tables were still readable so a power-loss on an SBC does
// not require --create-admin or a new remote-MCP token.
struct RecoverResult {
  bool rebuilt = false;
  bool adminRestored = false;
  bool mcpRestored = false;
  std::filesystem::path quarantined;
  std::vector<std::string> notes;
};

// Thin RAII wrapper around a single sqlite3 connection to the index db,
// plus the migration runner. This is the *only* index/db_path the process
// opens — SqliteIndex (search/documents) and, in wiki-server, the auth
// module (users/sessions) both operate on the same Database instance,
// since users/sessions/documents/FTS all live in one file per the plan's
// schema.
//
// The db is a disposable cache, never a second source of truth: if it's
// missing or its schema_version doesn't match, migrate() brings it up to
// date from nothing (an empty db is just "version 0"). Auth/MCP settings
// in the same file are NOT disposable — ensureUsable() exists so a
// corrupt index can be thrown away without throwing those away too.
class Database {
 public:
  explicit Database(std::filesystem::path dbPath);
  ~Database();

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  // Applies any migrations newer than the db's current schema_version, in
  // order, each inside its own transaction. Safe to call on an already
  // up-to-date db (no-op) or a brand-new empty file. wiki-mcp and unit
  // tests call this directly; wiki-server uses ensureUsable() instead
  // (migrate + integrity_check + auto-rebuild).
  void migrate();

  // wiki-server startup: migrate, PRAGMA integrity_check, and if the
  // file is damaged (typical: SD card yanked mid-write) quarantine it
  // and open a fresh schema, restoring users / sessions / remote-MCP /
  // embeddings_runtime_config / mcp_audit_log when those SELECTs still
  // work. Documents/FTS/embeddings are rebuilt from the vault by the
  // existing startup rescan. No-op on a healthy file. wiki-mcp does
  // NOT call this — clients spawn it often; the always-on wiki-server
  // is the process that actually survives a dirty shutdown.
  RecoverResult ensureUsable();

  sqlite3* handle() const noexcept { return db_; }

  int currentSchemaVersion() const;

  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  void openConnection();
  void closeConnection();
  std::vector<std::string> integrityErrors() const;
  RecoverResult rebuildPreservingAuth(const std::string& reason);
  std::filesystem::path quarantineFiles();
  void pruneOldQuarantines();

  std::filesystem::path path_;
  sqlite3* db_ = nullptr;
  std::filesystem::path constructorQuarantined_;
};

}  // namespace wikicore::index
