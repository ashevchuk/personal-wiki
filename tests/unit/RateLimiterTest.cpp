#include "auth/RateLimiter.h"

#include <catch2/catch_test_macros.hpp>

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
