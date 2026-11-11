#include "auth/RateLimiter.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

using wikicore::auth::RateLimiter;

TEST_CASE("RateLimiter allows a fresh key", "[RateLimiter]") {
  RateLimiter limiter;
  REQUIRE(limiter.allow("1.2.3.4"));
}

TEST_CASE("RateLimiter blocks immediately after a failure", "[RateLimiter]") {
  RateLimiter limiter;
  limiter.recordFailure("1.2.3.4");
  REQUIRE_FALSE(limiter.allow("1.2.3.4"));
}

TEST_CASE("RateLimiter tracks keys independently", "[RateLimiter]") {
  RateLimiter limiter;
  limiter.recordFailure("1.2.3.4");
  REQUIRE_FALSE(limiter.allow("1.2.3.4"));
  REQUIRE(limiter.allow("5.6.7.8"));
}

TEST_CASE("RateLimiter clears backoff on success", "[RateLimiter]") {
  RateLimiter limiter;
  limiter.recordFailure("1.2.3.4");
  REQUIRE_FALSE(limiter.allow("1.2.3.4"));
  limiter.recordSuccess("1.2.3.4");
  REQUIRE(limiter.allow("1.2.3.4"));
}

TEST_CASE("RateLimiter shares one bucket across an IPv6 /64", "[RateLimiter]") {
  RateLimiter limiter;
  limiter.recordFailure("2001:db8:1:2::1");
  REQUIRE_FALSE(limiter.allow("2001:db8:1:2:aaaa:bbbb:cccc:dddd"));
  REQUIRE_FALSE(limiter.allow("2001:0db8:0001:0002:0000:0000:0000:0001"));
  REQUIRE(limiter.allow("2001:db8:1:3::1"));
  limiter.recordSuccess("2001:db8:1:2::ffff");
  REQUIRE(limiter.allow("2001:db8:1:2::1"));
}

TEST_CASE("RateLimiter keeps IPv4 and IPv4-mapped addresses per address", "[RateLimiter]") {
  RateLimiter limiter;
  limiter.recordFailure("192.0.2.1");
  REQUIRE(limiter.allow("192.0.2.2"));
  limiter.recordFailure("::ffff:192.0.2.10");
  REQUIRE_FALSE(limiter.allow("::ffff:192.0.2.10"));
  REQUIRE(limiter.allow("::ffff:192.0.2.11"));
}

TEST_CASE("RateLimiter drops expired entries once it tracks too many keys", "[RateLimiter]") {
  RateLimiter limiter;
  for (size_t i = 0; i < RateLimiter::kMaxTrackedKeys; ++i) {
    limiter.recordFailure("key-" + std::to_string(i));
  }
  REQUIRE(limiter.trackedKeys() == RateLimiter::kMaxTrackedKeys);

  // Every entry above has a single failure, i.e. a one-second block.
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  limiter.recordFailure("one-more");
  REQUIRE(limiter.trackedKeys() == 1);
  REQUIRE_FALSE(limiter.allow("one-more"));
}
