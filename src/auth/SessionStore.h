#pragma once

#include "index/Database.h"

#include <cstdint>
#include <optional>
#include <string>

namespace wikicore::auth {

struct SessionInfo {
  int64_t userId;
  std::string csrfToken;
};

struct NewSession {
  std::string rawToken;   // set as the session cookie's value
  std::string csrfToken;  // embedded in forms/headers for CsrfFilter
};

// SQLite-backed sessions: survive a server restart, and store only a hash
// of the cookie token (SHA-256), never the raw value — a stolen copy of
// the db doesn't let anyone hijack an existing session.
class SessionStore {
 public:
  explicit SessionStore(index::Database& db) : db_(db) {}

  // 14-day default TTL — a personal single-admin wiki, not a bank.
  NewSession create(int64_t userId, const std::string& userAgent,
                     const std::string& ip, long ttlSeconds = 60 * 60 * 24 * 14);

  // nullopt if the token is unknown or expired. Bumps last_seen_at on a
  // successful hit.
  std::optional<SessionInfo> validate(const std::string& rawToken);

  void destroy(const std::string& rawToken);

  // Invalidates every OTHER session belonging to `userId` — used after a
  // password change, so a leaked/stolen session token elsewhere gets
  // kicked out immediately rather than surviving until its own natural
  // expiry. Keeps `keepRawToken`'s own session alive (the one that made
  // the change) so changing your own password doesn't also log you out.
  void destroyAllExcept(int64_t userId, const std::string& keepRawToken);

  // Opportunistic cleanup — called on create(), not on a timer (this is a
  // low-traffic personal service; a background sweep thread is
  // unnecessary complexity for what create() already does for free).
  void pruneExpired();

 private:
  index::Database& db_;
};

}  // namespace wikicore::auth
