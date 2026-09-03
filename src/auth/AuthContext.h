#pragma once

namespace wikicore::auth {

// Shared constants between AuthFilter, CsrfFilter, and AuthController —
// kept in one place so the cookie name / attribute keys / header name
// can't drift out of sync between the three.

inline constexpr const char* kSessionCookieName = "wiki_session";

// Readable (non-HttpOnly) delivery of the CSRF token to the client — the
// synchronizer token itself still lives server-side in `sessions` and
// CsrfFilter validates against THAT, never against this cookie's value
// directly; this is just how the client learns what to send back. Set
// alongside kSessionCookieName on login, cleared alongside it on logout.
inline constexpr const char* kCsrfCookieName = "wiki_csrf_token";

// Keys AuthFilter writes into req->attributes() and everything downstream
// (CsrfFilter, route handlers) reads from. userId is a
// std::optional<int64_t> (nullopt = not authenticated, the fail-safe
// default when the key is absent — see Attributes::get<T>).
inline constexpr const char* kAttrUserId = "auth.userId";
inline constexpr const char* kAttrCsrfToken = "auth.csrfToken";

// Where CsrfFilter looks for the token on a mutating request: this header
// first (htmx/JS requests), falling back to a "csrf_token" form field
// (classic HTML forms).
inline constexpr const char* kCsrfHeaderName = "X-CSRF-Token";
inline constexpr const char* kCsrfFormField = "csrf_token";

}  // namespace wikicore::auth
