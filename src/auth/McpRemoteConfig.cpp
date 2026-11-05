#include "auth/McpRemoteConfig.h"

#include "auth/CidrMatch.h"
#include "auth/Crypto.h"
#include "util/Time.h"

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

std::string columnText(sqlite3_stmt* stmt, int index) {
  const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, index));
  return text != nullptr ? std::string(text) : std::string();
}

}  // namespace

RemoteMcpSettings McpRemoteConfig::get() const {
  sqlite3_stmt* stmt =
      prepare(db_.handle(), "SELECT enabled, write_enabled "
                             "FROM mcp_remote_config WHERE id = 1;");
  RemoteMcpSettings settings;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    settings.enabled = sqlite3_column_int64(stmt, 0) != 0;
    settings.writeEnabled = sqlite3_column_int64(stmt, 1) != 0;
  }
  sqlite3_finalize(stmt);
  return settings;
}

void McpRemoteConfig::setEnabled(bool enabled) {
  // The ON CONFLICT branch only ever touches the ONE column each setter
  // owns -- write_enabled on an already-existing row is untouched here,
  // only defaulted on a genuinely first-ever INSERT (where 0 already IS
  // the correct starting state anyway).
  sqlite3_stmt* stmt =
      prepare(db_.handle(),
              "INSERT INTO mcp_remote_config(id, enabled, write_enabled) "
              "VALUES (1, ?1, 0) "
              "ON CONFLICT(id) DO UPDATE SET enabled = excluded.enabled;");
  sqlite3_bind_int64(stmt, 1, enabled ? 1 : 0);
  run(db_.handle(), stmt);
}

void McpRemoteConfig::setWriteEnabled(bool enabled) {
  sqlite3_stmt* stmt =
      prepare(db_.handle(),
              "INSERT INTO mcp_remote_config(id, enabled, write_enabled) "
              "VALUES (1, 0, ?1) "
              "ON CONFLICT(id) DO UPDATE SET write_enabled = excluded.write_enabled;");
  sqlite3_bind_int64(stmt, 1, enabled ? 1 : 0);
  run(db_.handle(), stmt);
}

std::vector<RemoteMcpToken> McpRemoteConfig::listTokens() const {
  sqlite3_stmt* stmt = prepare(
      db_.handle(),
      "SELECT id, label, token_prefix, created_at, last_used_at, agent_access "
      "FROM mcp_remote_tokens ORDER BY created_at DESC, id DESC;");
  std::vector<RemoteMcpToken> results;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    RemoteMcpToken t;
    t.id = sqlite3_column_int64(stmt, 0);
    t.label = columnText(stmt, 1);
    t.tokenPrefix = columnText(stmt, 2);
    t.createdAt = columnText(stmt, 3);
    t.lastUsedAt = columnText(stmt, 4);
    t.agentAccess = sqlite3_column_int64(stmt, 5) != 0;
    results.push_back(std::move(t));
  }
  sqlite3_finalize(stmt);
  return results;
}

McpRemoteConfig::NewToken McpRemoteConfig::createToken(const std::string& label) {
  const std::string raw = randomHexToken();
  sqlite3_stmt* stmt = prepare(
      db_.handle(),
      "INSERT INTO mcp_remote_tokens(label, token_hash, token_prefix, created_at, last_used_at, "
      "agent_access) VALUES (?1, ?2, ?3, ?4, NULL, 0);");
  bindText(stmt, 1, label);
  bindText(stmt, 2, sha256Hex(raw));
  bindText(stmt, 3, raw.substr(0, 8));
  bindText(stmt, 4, util::nowIso8601());
  run(db_.handle(), stmt);
  return NewToken{sqlite3_last_insert_rowid(db_.handle()), raw};
}

McpRemoteConfig::NewToken McpRemoteConfig::regenerateToken(int64_t id) {
  const std::string raw = randomHexToken();
  sqlite3_stmt* stmt = prepare(
      db_.handle(),
      "UPDATE mcp_remote_tokens SET token_hash = ?1, token_prefix = ?2, last_used_at = NULL "
      "WHERE id = ?3;");
  bindText(stmt, 1, sha256Hex(raw));
  bindText(stmt, 2, raw.substr(0, 8));
  sqlite3_bind_int64(stmt, 3, id);
  run(db_.handle(), stmt);
  return NewToken{id, raw};
}

void McpRemoteConfig::deleteToken(int64_t id) {
  sqlite3_stmt* stmt = prepare(db_.handle(), "DELETE FROM mcp_remote_tokens WHERE id = ?1;");
  sqlite3_bind_int64(stmt, 1, id);
  run(db_.handle(), stmt);
}

void McpRemoteConfig::setAgentAccess(int64_t id, bool enabled) {
  sqlite3_stmt* stmt = prepare(
      db_.handle(), "UPDATE mcp_remote_tokens SET agent_access = ?1 WHERE id = ?2;");
  sqlite3_bind_int64(stmt, 1, enabled ? 1 : 0);
  sqlite3_bind_int64(stmt, 2, id);
  run(db_.handle(), stmt);
}

McpRemoteConfig::TokenMatch McpRemoteConfig::verifyToken(const std::string& rawToken) {
  if (rawToken.empty()) return TokenMatch{};
  const std::string hash = sha256Hex(rawToken);

  // Constant-time compare per row, not `==` — this gates authentication
  // for an anonymous caller on the public-internet remote MCP transport
  // (see docs/architecture.md, "Remote MCP transport (HTTP)"), the
  // highest-stakes credential check in this codebase. Every row is
  // compared, with no early `break` on the first match, so total
  // latency never depends on which token (if any) matched.
  sqlite3_stmt* stmt = prepare(
      db_.handle(), "SELECT id, label, token_hash, agent_access FROM mcp_remote_tokens;");
  int64_t matchedId = 0;
  TokenMatch match;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const std::string storedHash = columnText(stmt, 2);
    if (storedHash.empty()) continue;
    if (constantTimeEquals(hash, storedHash)) {
      match.matched = true;
      matchedId = sqlite3_column_int64(stmt, 0);
      match.label = columnText(stmt, 1);
      match.agentAccess = sqlite3_column_int64(stmt, 3) != 0;
    }
  }
  sqlite3_finalize(stmt);

  if (!match.matched) return match;

  sqlite3_stmt* update =
      prepare(db_.handle(), "UPDATE mcp_remote_tokens SET last_used_at = ?1 WHERE id = ?2;");
  bindText(update, 1, util::nowIso8601());
  sqlite3_bind_int64(update, 2, matchedId);
  run(db_.handle(), update);

  return match;
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
