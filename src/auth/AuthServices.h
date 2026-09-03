#pragma once

#include "auth/AdminAccount.h"
#include "auth/RateLimiter.h"
#include "auth/SessionStore.h"

namespace wikicore::auth {

// Process-wide handles to the auth services, set once at startup.
//
// Why this exists: Drogon auto-instantiates HttpFilter subclasses itself,
// via a no-argument default constructor (DrObject reflection) — there is
// no constructor-injection path for filters. This is the standard,
// narrowly-scoped way to hand a filter shared state that outlives any
// single request; it is not a general-purpose singleton, and nothing
// outside auth/ and the filters is meant to reach through it. Route
// handlers get their services passed explicitly instead, since they
// (unlike filters) are ordinary lambdas we construct ourselves.
class AuthServices {
 public:
  static void init(SessionStore& sessions, RateLimiter& rateLimiter,
                    AdminAccount& admin) {
    sessions_ = &sessions;
    rateLimiter_ = &rateLimiter;
    admin_ = &admin;
  }

  static SessionStore& sessions() { return *sessions_; }
  static RateLimiter& rateLimiter() { return *rateLimiter_; }
  static AdminAccount& admin() { return *admin_; }

 private:
  static SessionStore* sessions_;
  static RateLimiter* rateLimiter_;
  static AdminAccount* admin_;
};

}  // namespace wikicore::auth
