#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace wikicore::auth {

// In-memory exponential-backoff limiter for /login. Keyed by client IP
// (the plan calls for "IP+username", but username is exactly what an
// attacker is trying to guess — keying on it too would let them test
// distinct usernames in parallel without ever tripping the limiter for a
// single one; IP alone doesn't have that hole, and this is a
// single-admin-account service, so IP is already unambiguous).
//
// In-memory and not persisted across restarts: acceptable here (this is
// a personal service, not a bank) and avoids a sessions-table-sized
// amount of ceremony for what a restart-and-reset every so often doesn't
// meaningfully weaken.
//
// Keys are client addresses. An IPv6 address is bucketed by its /64 —
// the smallest block a single subscriber normally controls — so cycling
// through addresses inside one prefix does not hand out a fresh bucket
// each time. IPv4 (and IPv4-mapped IPv6) stays one bucket per address.
class RateLimiter {
 public:
  // Above this many tracked keys, recordFailure drops every entry whose
  // block has already expired, so a spray of distinct addresses cannot
  // grow the map without bound.
  static constexpr size_t kMaxTrackedKeys = 10000;

  // False if `key` is currently locked out.
  bool allow(const std::string& key);

  void recordFailure(const std::string& key);
  void recordSuccess(const std::string& key);

  size_t trackedKeys();

 private:
  struct Entry {
    int consecutiveFailures = 0;
    std::chrono::steady_clock::time_point blockedUntil{};
  };

  std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
};

}  // namespace wikicore::auth
