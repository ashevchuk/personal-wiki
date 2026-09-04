#pragma once

#include <drogon/HttpRequest.h>

#include <string>

namespace wikicore::auth {

// The caller's REAL IP, for the two things that actually need it against
// a real attacker rather than a trusted internal caller: the remote MCP
// rate limiter and its IP allowlist (McpRemoteConfig::isIpAllowed).
//
// `req->getPeerAddr()` (used by /login's own rate limiter today) is the
// raw TCP peer — behind the reverse proxy this app is documented to run
// behind for any public exposure (docs/deployment.md), that's ALWAYS the
// proxy's own loopback address, never the actual client. An IP allowlist
// checked against that would either allow everyone (if the proxy's
// address happens to be in the list) or nobody, and a rate limiter keyed
// on it becomes one shared bucket for every visitor rather than one per
// attacker — silently useless in exactly the deployment shape this
// feature is FOR.
//
// Reads X-Real-IP FIRST, falling back to X-Forwarded-For's LAST entry,
// falling back to the raw peer address when neither header is present
// (hit directly, no proxy) — verified against this app's own real,
// deployed nginx config (docs/deployment.md), not assumed:
//
//   proxy_set_header X-Real-IP $remote_addr;
//   proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
//
// `proxy_set_header` OVERWRITES a header before forwarding it upstream
// — no client-supplied X-Real-IP survives that, `$remote_addr` is
// nginx's own view of the TCP peer, unspoofable from the client side.
// `$proxy_add_x_forwarded_for`, by contrast, APPENDS $remote_addr to
// whatever X-Forwarded-For the client already sent — so the FIRST entry
// in the resulting header is exactly what the client claimed (trivially
// spoofable: curl -H "X-Forwarded-For: 127.0.0.1" bypasses an allowlist
// checking that entry), while the LAST entry is always nginx's own
// append, exactly as trustworthy as X-Real-IP. An earlier version of
// this function read X-Forwarded-For's FIRST entry as the trusted one —
// backwards for this exact, real config; caught by reading the actual
// deployed nginx file instead of assuming a convention.
//
// This still trusts whatever headers arrive at all — it is NOT safe if
// something in front of this app forwards a client-supplied header
// without normalizing it the way the directives above do. A deployment
// with a DIFFERENT proxy chain (more than one hop, or a proxy that
// doesn't set X-Real-IP) needs to verify its own directives produce the
// same guarantee before relying on the IP allowlist for anything.
std::string clientIp(const drogon::HttpRequestPtr& req);

}  // namespace wikicore::auth
