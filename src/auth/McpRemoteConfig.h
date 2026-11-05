#pragma once

#include "index/Database.h"

#include <cstdint>
#include <string>
#include <vector>

namespace wikicore::auth {

struct RemoteMcpSettings {
  bool enabled = false;
  bool writeEnabled = false;
};

// One row of mcp_remote_tokens, for display only — never the raw token or
// its hash (see McpRemoteConfig below).
struct RemoteMcpToken {
  int64_t id = 0;
  std::string label;
  std::string tokenPrefix;  // first 8 hex chars of the raw value, cosmetic
  std::string createdAt;
  std::string lastUsedAt;  // "" if never used
  // Reserved for a not-yet-built remote agent endpoint (free-text input,
  // the model itself decides what to write) -- a materially different
  // risk from the deterministic create_document/update_document/
  // attach_file tools, so it's gated per-token rather than folded into
  // the global writeEnabled toggle every token already shares. Currently
  // has no effect anywhere: nothing reads this flag until that endpoint
  // exists.
  bool agentAccess = false;
};

// Runtime-mutable settings for the remote (HTTP) MCP transport —
// deliberately SQLite-backed, not config.toml, because the whole point
// is the admin toggling enable/disable, write access, the IP allowlist,
// and now individual tokens from the Web UI without a server restart
// (see RemoteMcpRoutes.cpp for the actual transport, AdminRoutes.cpp/the
// account page for the settings UI).
//
// Multiple independently-labeled tokens, not one singleton — each client
// (Claude Desktop, a Claude Code remote connection, a future standalone
// client) gets its own, so revoking one doesn't take down the others and
// mcp_audit_log can say which one made a given call. Deliberately NOT
// per-token permissions or per-token IP allowlists: this project is
// "one admin, no roles" everywhere else, and a permission matrix per
// token would contradict that for no real benefit on a single-admin
// instance — enabled/writeEnabled/the CIDR allowlist stay global, shared
// by every token.
//
// Same discipline as SessionStore for every token's secret: only its
// SHA-256 hash is ever persisted (auth/Crypto.h) — a stolen copy of the
// db doesn't hand over anything usable, and createToken()/
// regenerateToken() are the ONLY places a raw value is ever visible, for
// exactly one return value each.
class McpRemoteConfig {
 public:
  struct NewToken {
    int64_t id = 0;
    std::string rawToken;
  };
  struct TokenMatch {
    bool matched = false;
    std::string label;
    bool agentAccess = false;
  };

  explicit McpRemoteConfig(index::Database& db) : db_(db) {}

  RemoteMcpSettings get() const;
  void setEnabled(bool enabled);
  void setWriteEnabled(bool enabled);

  // Newest first.
  std::vector<RemoteMcpToken> listTokens() const;

  // Generates a fresh random token, persists only its hash (+ a cosmetic
  // 8-char prefix of the raw value), and returns the raw value — shown to
  // the admin once, on this call, in the Web UI response.
  NewToken createToken(const std::string& label);

  // Same row (same label/id), fresh secret — the old raw value stops
  // working immediately, same as changing a password invalidates old
  // sessions. last_used_at resets to "never" since nothing has used the
  // new secret yet.
  NewToken regenerateToken(int64_t id);

  void deleteToken(int64_t id);

  // Reserved for the future agent endpoint (see RemoteMcpToken::agentAccess
  // above) -- independent of the token's label/secret, so flipping it
  // never invalidates the existing bearer value.
  void setAgentAccess(int64_t id, bool enabled);

  // Checks `rawToken` against every active token's hash — constant-time
  // per comparison, and the loop never short-circuits on the first match,
  // so total latency doesn't depend on which token (if any) matched. On a
  // match, stamps that token's last_used_at in the same call (this is the
  // single choke point every /mcp request passes through,
  // RemoteMcpRoutes.cpp — nowhere else needs to touch it) and returns its
  // label for audit attribution. Never throws on a malformed/empty token
  // — "doesn't match" is the correct answer for that, not an error.
  TokenMatch verifyToken(const std::string& rawToken);

  // Newest first.
  std::vector<std::string> listAllowedCidrs() const;
  void addAllowedCidr(const std::string& cidr);
  void removeAllowedCidr(const std::string& cidr);

  // True if `ip` matches any entry in the allowlist, OR the allowlist is
  // EMPTY. An empty list means "no IP restriction configured" (allow),
  // not "nothing is allowed" — the bearer token is the primary gate here;
  // the IP allowlist is an optional additional layer an admin opts INTO
  // by adding entries, not a default-deny nobody asked for. Delegates
  // the actual matching to CidrMatch.h.
  bool isIpAllowed(const std::string& ip) const;

 private:
  index::Database& db_;
};

}  // namespace wikicore::auth
