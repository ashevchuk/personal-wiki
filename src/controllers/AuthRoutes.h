#pragma once

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

// Registers GET/POST /login and POST /logout. Must be called after
// wikicore::auth::AuthServices::init() — the handlers reach through it
// for SessionStore/RateLimiter/AdminAccount.
void registerAuthRoutes(drogon::HttpAppFramework& app);

}  // namespace wikicore::controllers
