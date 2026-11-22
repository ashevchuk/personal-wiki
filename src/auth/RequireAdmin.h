#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <optional>

namespace wikicore::auth {

bool isAuthenticated(const drogon::HttpRequestPtr& req);

// CsrfFilter deliberately passes an unauthenticated request straight
// through (see CsrfFilter.h) — it treats "no session" as an authentication
// problem, not a CSRF one, and leaves it for the handler to reject. Every
// mutating handler MUST call this first: AuthFilter only annotates the
// request, it never blocks anything itself either. Skipping this call on
// any handler is a full unauthenticated write — see docs/architecture.md,
// "Request handling and auth".
//
// Returns the 401 response to send if unauthenticated, nullopt otherwise.
std::optional<drogon::HttpResponsePtr> requireAdminApi(const drogon::HttpRequestPtr& req);

}  // namespace wikicore::auth
