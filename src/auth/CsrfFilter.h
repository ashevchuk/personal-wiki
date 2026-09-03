#pragma once

#include <drogon/HttpFilter.h>

namespace wikicore::auth {

// Applied to mutating routes (POST/PUT/PATCH/DELETE) that require an
// existing session — everywhere except /login, which is rate-limited
// instead (there's no session yet to hold a CSRF token against). If the
// request carries no valid session at all, this filter passes it through
// unchanged: that's an authentication failure, which is each handler's
// own concern, not this filter's. If a valid session exists, the
// submitted token (X-CSRF-Token header, falling back to a csrf_token form
// field) must match the session's csrf_token or the request is rejected
// with 403 before the handler ever runs.
class CsrfFilter : public drogon::HttpFilter<CsrfFilter> {
 public:
  void doFilter(const drogon::HttpRequestPtr& req,
                drogon::FilterCallback&& fcb,
                drogon::FilterChainCallback&& fccb) override;
};

}  // namespace wikicore::auth
