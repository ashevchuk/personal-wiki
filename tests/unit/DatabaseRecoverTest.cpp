#include "index/Database.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using wikicore::index::Database;

namespace {

class TempDb {
 public:
  TempDb()
      : path_(fs::temp_directory_path() /
              fs::path("wiki-db-recover-test-" +
                       std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".db")) {
    cleanup();
  }
  ~TempDb() { cleanup(); }
  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;
  const fs::path& path() const { return path_; }

  std::vector<fs::path> quarantineFiles() const {
    std::vector<fs::path> out;
    const std::string prefix = path_.filename().string() + ".corrupt-";
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(path_.parent_path(), ec)) {
      if (entry.path().filename().string().rfind(prefix, 0) == 0) out.push_back(entry.path());
    }
    return out;
  }

 private:
  void cleanup() {
    std::error_code ec;
    fs::remove(path_, ec);
    fs::remove(fs::path(path_.string() + "-wal"), ec);
    fs::remove(fs::path(path_.string() + "-shm"), ec);
    for (const auto& p : quarantineFiles()) fs::remove(p, ec);
    fs::remove(fs::path(path_.string() + "-wal.corrupt-dummy"), ec);
  }

  fs::path path_;
};

void exec(sqlite3* db, const char* sql) {
  char* err = nullptr;
  REQUIRE(sqlite3_exec(db, sql, nullptr, nullptr, &err) == SQLITE_OK);
  sqlite3_free(err);
}

std::string scalar(sqlite3* db, const char* sql) {
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
  const auto* t = sqlite3_column_text(stmt, 0);
  std::string out = t ? reinterpret_cast<const char*>(t) : "";
  sqlite3_finalize(stmt);
  return out;
}

int countRows(sqlite3* db, const char* sql) {
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
  const int n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

void seedPreciousRows(Database& db) {
  exec(db.handle(),
       "INSERT INTO users(id, username, password_hash, created_at) "
       "VALUES (1, 'admin', 'argon2-hash-keep-me', '2026-01-01T00:00:00Z');");
  exec(db.handle(),
       "INSERT INTO sessions(token_hash, user_id, created_at, expires_at, last_seen_at, csrf_token) "
       "VALUES ('sess-hash', 1, '2026-01-01T00:00:00Z', '2099-01-01T00:00:00Z', "
       "'2026-01-01T00:00:00Z', 'csrf-keep');");
  exec(db.handle(),
       "INSERT INTO mcp_remote_config(id, enabled, write_enabled, token_hash) "
       "VALUES (1, 1, 1, 'mcp-token-hash-keep');");
  exec(db.handle(), "INSERT INTO mcp_remote_allowed_cidrs(cidr) VALUES ('10.100.100.0/24');");
  exec(db.handle(),
       "INSERT INTO embeddings_runtime_config(id, vector_search_enabled) VALUES (1, 0);");
  exec(db.handle(),
       "INSERT INTO agent_chats(id, title, created_at, updated_at, events_json, messages_json) "
       "VALUES ('chat-keep', 'Keep me', '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z', "
       "'[]', '[]');");
  // Enough rows that the file has pages past the header, so overwriting
  // the tail corrupts data without destroying the users table.
  for (int i = 0; i < 200; ++i) {
    exec(db.handle(),
         "INSERT INTO mcp_audit_log(at, tool_name, path, success, detail) "
         "VALUES ('2026-01-01T00:00:00Z', 'create_document', 'notes/pad.md', 1, 'bulk');");
  }
  exec(db.handle(), "PRAGMA wal_checkpoint(TRUNCATE);");
}

void corruptTail(const fs::path& path) {
  std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
  REQUIRE(f.good());
  f.seekg(0, std::ios::end);
  const auto size = static_cast<std::streamoff>(f.tellg());
  REQUIRE(size > 2048);
  f.seekp(size - 2048);
  const std::string junk(2048, '\xff');
  f.write(junk.data(), static_cast<std::streamsize>(junk.size()));
  REQUIRE(f.good());
}

}  // namespace

TEST_CASE("ensureUsable is a no-op on a healthy db and does not quarantine",
          "[Database]") {
  TempDb tmp;
  Database db(tmp.path());
  const auto first = db.ensureUsable();
  REQUIRE_FALSE(first.rebuilt);
  seedPreciousRows(db);

  const auto second = db.ensureUsable();
  REQUIRE_FALSE(second.rebuilt);
  REQUIRE(tmp.quarantineFiles().empty());
  REQUIRE(scalar(db.handle(), "SELECT username FROM users WHERE id = 1") == "admin");
}

TEST_CASE("ensureUsable rebuilds a torn db and restores admin + remote MCP",
          "[Database]") {
  TempDb tmp;
  {
    Database db(tmp.path());
    db.ensureUsable();
    seedPreciousRows(db);
  }
  corruptTail(tmp.path());

  Database db(tmp.path());
  const auto recovered = db.ensureUsable();
  REQUIRE(recovered.rebuilt);
  REQUIRE(recovered.adminRestored);
  REQUIRE(recovered.mcpRestored);
  REQUIRE_FALSE(recovered.quarantined.empty());
  REQUIRE(fs::exists(recovered.quarantined));
  REQUIRE(scalar(db.handle(), "SELECT username FROM users WHERE id = 1") == "admin");
  REQUIRE(scalar(db.handle(), "SELECT password_hash FROM users WHERE id = 1") ==
          "argon2-hash-keep-me");
  REQUIRE(scalar(db.handle(), "SELECT csrf_token FROM sessions") == "csrf-keep");
  REQUIRE(scalar(db.handle(), "SELECT token_hash FROM mcp_remote_config WHERE id = 1") ==
          "mcp-token-hash-keep");
  REQUIRE(scalar(db.handle(), "SELECT cidr FROM mcp_remote_allowed_cidrs") == "10.100.100.0/24");
  REQUIRE(scalar(db.handle(),
                 "SELECT vector_search_enabled FROM embeddings_runtime_config WHERE id = 1") ==
          "0");
  // Tail-page audit rows sitting in the overwritten 2 KiB are unreadable;
  // the rest (and every auth/MCP row, which live on earlier pages) survive.
  REQUIRE(countRows(db.handle(), "SELECT COUNT(*) FROM mcp_audit_log") > 0);
  REQUIRE(scalar(db.handle(), "SELECT title FROM agent_chats WHERE id = 'chat-keep'") ==
          "Keep me");
}

TEST_CASE("a file that is not SQLite at all is quarantined and replaced, "
          "without requiring a manual delete",
          "[Database]") {
  TempDb tmp;
  {
    std::ofstream(tmp.path()) << "this is not a database";
  }
  Database db(tmp.path());
  const auto recovered = db.ensureUsable();
  REQUIRE(recovered.rebuilt);
  REQUIRE_FALSE(recovered.adminRestored);
  REQUIRE(db.currentSchemaVersion() == 6);
  REQUIRE(countRows(db.handle(), "SELECT COUNT(*) FROM users") == 0);
}
