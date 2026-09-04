#pragma once

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

// Registers the auth JSON API. Must be called after
// wikicore::auth::AuthServices::init() — the handlers reach through it
// for SessionStore/RateLimiter/AdminAccount.
//
//   POST /api/login    {username, password} -> {ok:true} + session/csrf
//                       cookies, or {ok:false, error} with 401/429
//   POST /api/logout    -> {ok:true}, clears cookies (CSRF-protected)
//   GET  /api/session   -> {authenticated: bool}
//   POST /api/account/password  {currentPassword, newPassword} -> {ok:true}
//                       (admin+CSRF protected). Verifies currentPassword
//                       via PasswordHasher before overwriting the stored
//                       hash, then destroys every OTHER session for this
//                       account (see SessionStore::destroyAllExcept) —
//                       the caller's own session survives so this doesn't
//                       also log the caller out.
//
// No HTML here at all — see docs/architecture.md's frontend section: the
// client (static/js/pages/login.js) owns the login form and redirect
// entirely; this only ever returns JSON. No basePath parameter either —
// unlike the old HTML-returning version, nothing here builds a URL/link
// that would need it (the client already knows its own base path, see
// common.js's basePath()).
void registerAuthRoutes(drogon::HttpAppFramework& app);

}  // namespace wikicore::controllers
