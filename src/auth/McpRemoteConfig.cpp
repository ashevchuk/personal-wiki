#include "auth/McpRemoteConfig.h"

#include "auth/CidrMatch.h"
#include "auth/Crypto.h"

#include <sqlite3.h>

#include <stdexcept>

namespace wikicore::auth {

namespace {

void bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
  sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

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

RemoteMcpSettings McpRemoteConfig::get() const {
  sqlite3_stmt* stmt =
      prepare(db_.handle(), "SELECT enabled, write_enabled, token_hash "
                             "FROM mcp_remote_config WHERE id = 1;");
  RemoteMcpSettings settings;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    settings.enabled = sqlite3_column_int64(stmt, 0) != 0;
    settings.writeEnabled = sqlite3_column_int64(stmt, 1) != 0;
    const auto* tokenHash = sqlite3_column_text(stmt, 2);
    settings.hasToken = tokenHash != nullptr && tokenHash[0] != '\0';
  }
  sqlite3_finalize(stmt);
  return settings;
}

void McpRemoteConfig::setEnabled(bool enabled) {
  // The ON CONFLICT branch only ever touches the ONE column each setter
  // owns -- write_enabled/token_hash on an already-existing row are
  // untouched here, only defaulted on a genuinely first-ever INSERT
  // (where 0/NULL already IS the correct starting state anyway).
  sqlite3_stmt* stmt =
      prepare(db_.handle(),
              "INSERT INTO mcp_remote_config(id, enabled, write_enabled, token_hash) "
              "VALUES (1, ?1, 0, NULL) "
              "ON CONFLICT(id) DO UPDATE SET enabled = excluded.enabled;");
  sqlite3_bind_int64(stmt, 1, enabled ? 1 : 0);
  run(db_.handle(), stmt);
}

void McpRemoteConfig::setWriteEnabled(bool enabled) {
  sqlite3_stmt* stmt =
      prepare(db_.handle(),
              "INSERT INTO mcp_remote_config(id, enabled, write_enabled, token_hash) "
              "VALUES (1, 0, ?1, NULL) "
              "ON CONFLICT(id) DO UPDATE SET write_enabled = excluded.write_enabled;");
  sqlite3_bind_int64(stmt, 1, enabled ? 1 : 0);
  run(db_.handle(), stmt);
}

std::string McpRemoteConfig::regenerateToken() {
  const std::string raw = randomHexToken();
  const std::string hash = sha256Hex(raw);
  sqlite3_stmt* stmt =
      prepare(db_.handle(),
              "INSERT INTO mcp_remote_config(id, enabled, write_enabled, token_hash) "
              "VALUES (1, 0, 0, ?1) "
              "ON CONFLICT(id) DO UPDATE SET token_hash = excluded.token_hash;");
  bindText(stmt, 1, hash);
  run(db_.handle(), stmt);
  return raw;
}

bool McpRemoteConfig::verifyToken(const std::string& rawToken) const {
  if (rawToken.empty()) return false;
  const std::string hash = sha256Hex(rawToken);

  sqlite3_stmt* stmt = prepare(
      db_.handle(), "SELECT token_hash FROM mcp_remote_config WHERE id = 1;");
  bool matches = false;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* stored = sqlite3_column_text(stmt, 0);
    if (stored != nullptr) {
      matches = hash == reinterpret_cast<const char*>(stored);
    }
  }
  sqlite3_finalize(stmt);
  return matches;
}

std::vector<std::string> McpRemoteConfig::listAllowedCidrs() const {
  sqlite3_stmt* stmt = prepare(
      db_.handle(), "SELECT cidr FROM mcp_remote_allowed_cidrs ORDER BY id;");
  std::vector<std::string> results;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    results.emplace_back(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
  }
  sqlite3_finalize(stmt);
  return results;
}

void McpRemoteConfig::addAllowedCidr(const std::string& cidr) {
  sqlite3_stmt* stmt = prepare(
      db_.handle(), "INSERT OR IGNORE INTO mcp_remote_allowed_cidrs(cidr) VALUES (?1);");
  bindText(stmt, 1, cidr);
  run(db_.handle(), stmt);
}

void McpRemoteConfig::removeAllowedCidr(const std::string& cidr) {
  sqlite3_stmt* stmt = prepare(
      db_.handle(), "DELETE FROM mcp_remote_allowed_cidrs WHERE cidr = ?1;");
  bindText(stmt, 1, cidr);
  run(db_.handle(), stmt);
}

bool McpRemoteConfig::isIpAllowed(const std::string& ip) const {
  const auto cidrs = listAllowedCidrs();
  if (cidrs.empty()) return true;  // see the header's own comment on why
  for (const auto& cidr : cidrs) {
    if (cidrContains(cidr, ip)) return true;
  }
  return false;
}

}  // namespace wikicore::auth
