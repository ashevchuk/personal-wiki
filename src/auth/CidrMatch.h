#pragma once

#include <string>

namespace wikicore::auth {

// True if `ip` (a plain IPv4 or IPv6 address string) falls within
// `cidr` ("a.b.c.d/n", "ipv6::addr/n", or a bare address treated as
// /32 or /128). Handles both families via inet_pton — an IPv4 `cidr`
// never matches an IPv6 `ip` and vice versa (no v4-mapped-v6 coercion,
// deliberately: guessing at that equivalence is exactly the kind of
// subtlety that turns an allowlist into an accidental bypass).
//
// Returns false (never throws) for anything malformed — an unparseable
// CIDR/IP is "doesn't match", not an error to propagate; the caller
// (McpRemoteConfig::isIpAllowed) folds that into its own fail-safe
// default the same way everything else in this app treats malformed
// input as the safe outcome, not a crash.
bool cidrContains(const std::string& cidr, const std::string& ip);

}  // namespace wikicore::auth
