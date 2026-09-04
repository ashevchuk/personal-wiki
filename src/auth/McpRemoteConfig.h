#pragma once

#include "index/Database.h"

#include <string>
#include <vector>

namespace wikicore::auth {

struct RemoteMcpSettings {
  bool enabled = false;
  bool writeEnabled = false;
  bool hasToken = false;  // never the raw token or its hash — see below
};

// Runtime-mutable settings for the remote (HTTP) MCP transport —
// deliberately SQLite-backed, not config.toml, because the whole point
// is the admin toggling enable/disable, write access, and the IP
// allowlist from the Web UI without a server restart (see
// RemoteMcpRoutes.cpp for the actual transport, AdminRoutes.cpp/the
// account page for the settings UI).
//
// Same discipline as SessionStore for the bearer token: only its SHA-256
// hash is ever persisted (auth/Crypto.h) — a stolen copy of the db
// doesn't hand over anything usable, and regenerateToken() is the ONLY
// place the raw value is ever visible, for exactly one return value.
class McpRemoteConfig {
 public:
  explicit McpRemoteConfig(index::Database& db) : db_(db) {}

  RemoteMcpSettings get() const;
  void setEnabled(bool enabled);
  void setWriteEnabled(bool enabled);

  // Generates a fresh random token, persists only its hash, and returns
  // the RAW value — shown to the admin once, on this call, in the Web
  // UI response. Overwrites any previously-issued token (immediate,
  // silent revocation of the old one, same as changing a password
  // invalidates old sessions).
  std::string regenerateToken();

  // False if no token has ever been generated, OR `rawToken` doesn't
  // match the current hash. Never throws on a malformed/empty token —
  // "doesn't match" is the correct answer for that, not an error.
  bool verifyToken(const std::string& rawToken) const;

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
