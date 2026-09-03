#pragma once

#include <drogon/HttpFilter.h>

namespace wikicore::auth {

// Runs on every request that lists it. Never blocks anything itself — it
// only reads the session cookie (if any) and writes the result into
// req->attributes() under kAttrUserId / kAttrCsrfToken (see
// AuthContext.h) for downstream filters/handlers to read. Absence of a
// valid session means the attribute comes back at its default
// (std::optional<int64_t>{} — not authenticated), which is the fail-safe
// direction: a handler that forgets to check it treats the request as
// anonymous, not as admin.
//
// Authorization decisions (private doc -> 404 for anon, admin-only
// routes) are each handler's own responsibility, not this filter's — see
// docs/architecture.md.
class AuthFilter : public drogon::HttpFilter<AuthFilter> {
 public:
  void doFilter(const drogon::HttpRequestPtr& req,
                drogon::FilterCallback&& fcb,
                drogon::FilterChainCallback&& fccb) override;
};

}  // namespace wikicore::auth
