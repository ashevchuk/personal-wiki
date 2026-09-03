#include "auth/RateLimiter.h"

#include <algorithm>

namespace wikicore::auth {

namespace {
constexpr int kMaxBackoffSeconds = 300;  // 5 minutes
}

bool RateLimiter::allow(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = entries_.find(key);
  if (it == entries_.end()) return true;
  return std::chrono::steady_clock::now() >= it->second.blockedUntil;
}

void RateLimiter::recordFailure(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry& entry = entries_[key];
  ++entry.consecutiveFailures;

  // 1, 2, 4, 8, ... seconds, capped — first failure barely slows anyone
  // down, repeated failures back off hard without a hard lockout.
  const int backoffSeconds = static_cast<int>(std::min<long long>(
      kMaxBackoffSeconds, 1LL << std::min(entry.consecutiveFailures - 1, 20)));
  entry.blockedUntil =
      std::chrono::steady_clock::now() + std::chrono::seconds(backoffSeconds);
}

void RateLimiter::recordSuccess(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.erase(key);
}

}  // namespace wikicore::auth
