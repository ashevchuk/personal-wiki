#include "auth/RateLimiter.h"

#include <arpa/inet.h>

#include <algorithm>
#include <cstdio>

namespace wikicore::auth {

namespace {
constexpr int kMaxBackoffSeconds = 300;  // 5 minutes

std::string bucketFor(const std::string& key) {
  in6_addr addr{};
  if (inet_pton(AF_INET6, key.c_str(), &addr) != 1 || IN6_IS_ADDR_V4MAPPED(&addr)) {
    return key;
  }
  char prefix[32];
  std::snprintf(prefix, sizeof(prefix), "%02x%02x:%02x%02x:%02x%02x:%02x%02x::/64",
                addr.s6_addr[0], addr.s6_addr[1], addr.s6_addr[2], addr.s6_addr[3],
                addr.s6_addr[4], addr.s6_addr[5], addr.s6_addr[6], addr.s6_addr[7]);
  return prefix;
}

}  // namespace

bool RateLimiter::allow(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = entries_.find(bucketFor(key));
  if (it == entries_.end()) return true;
  return std::chrono::steady_clock::now() >= it->second.blockedUntil;
}

void RateLimiter::recordFailure(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = std::chrono::steady_clock::now();
  if (entries_.size() >= kMaxTrackedKeys) {
    std::erase_if(entries_, [now](const auto& item) { return item.second.blockedUntil <= now; });
  }
  Entry& entry = entries_[bucketFor(key)];
  ++entry.consecutiveFailures;

  // 1, 2, 4, 8, ... seconds, capped — first failure barely slows anyone
  // down, repeated failures back off hard without a hard lockout.
  const int backoffSeconds = static_cast<int>(std::min<long long>(
      kMaxBackoffSeconds, 1LL << std::min(entry.consecutiveFailures - 1, 20)));
  entry.blockedUntil = now + std::chrono::seconds(backoffSeconds);
}

void RateLimiter::recordSuccess(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.erase(bucketFor(key));
}

size_t RateLimiter::trackedKeys() {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

}  // namespace wikicore::auth
