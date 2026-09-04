#pragma once

#include "auth/McpRemoteConfig.h"
#include "auth/RateLimiter.h"
#include "index/FtsSearch.h"
#include "index/IndexUpdater.h"
#include "index/McpAuditLog.h"
#include "index/NavQueries.h"
#include "vault/DocumentService.h"

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

// POST /mcp — the remote (public, HTTP) MCP transport, admin-toggleable
// at runtime via McpRemoteConfig (Web UI: AdminRoutes.cpp's
// /api/admin/mcp-remote-config routes + the Account page).
//
// Deliberately hand-built here rather than using the vendored cpp-mcp
// library's own HTTP+SSE server (::mcp::server::start()) — confirmed by
// reading mcp_server.cpp directly that its set_auth_handler() is set but
// NEVER INVOKED anywhere in the request path, i.e. an unpatched dead-code
// auth hook. Building on it and trusting that hook for a PUBLIC endpoint
// would ship something that looks token-protected and isn't. This
// implementation reuses only the underlying wikicore services
// (FtsSearch/NavQueries/DocumentService/IndexUpdater), gated by this
// app's own real, tested Drogon-level session-adjacent machinery
// (McpRemoteConfig + a dedicated RateLimiter), independent of the stdio
// server in McpServer.cpp (which stays untouched — a different, already-
// working transport for a different, already-trusted context).
//
// A single stateless request/response endpoint (MCP's "Streamable HTTP"
// transport, minus its optional Mcp-Session-Id — every tool here is a
// fast, synchronous call with nothing to carry across requests, so there
// is no session state worth tracking). Every request is independently
// authenticated: bearer token (Authorization: Bearer <token>) AND, if
// any allowlist entries are configured, the caller's IP (auth::clientIp
// — X-Real-IP/X-Forwarded-For-aware, NOT the raw TCP peer, which behind
// the reverse proxy this needs to run behind would always be the
// proxy's own address; see auth/ClientIp.h for exactly which header
// wins and why, verified against this deployment's own real nginx
// config, not assumed). When the feature is off
// (McpRemoteConfig::get().enabled == false, the default), every request
// gets a plain 404 — indistinguishable from the route never having
// existed, not a "this exists but needs a token" hint to an
// unauthenticated prober.
void registerRemoteMcpRoutes(drogon::HttpAppFramework& app,
                              wikicore::auth::McpRemoteConfig& remoteConfig,
                              wikicore::auth::RateLimiter& rateLimiter,
                              wikicore::index::FtsSearch& search,
                              wikicore::index::NavQueries& nav,
                              wikicore::index::IndexUpdater& indexUpdater,
                              wikicore::vault::DocumentService& documents,
                              wikicore::index::McpAuditLog& auditLog);

}  // namespace wikicore::controllers
