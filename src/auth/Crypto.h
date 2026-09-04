#pragma once

#include <string>

namespace wikicore::auth {

// Shared token/hash primitives — extracted out of SessionStore.cpp
// (which had this exact code first) so McpRemoteConfig can use the SAME
// discipline for its own bearer token (store only a hash, never the raw
// value — a stolen copy of the db doesn't hand over anything usable)
// without a second copy of OpenSSL glue code.
//
// Lives in src/auth/, not src/util/ — wikicore is deliberately free of
// any OpenSSL dependency (see CLAUDE.md's architecture section), and
// this needs it. Both McpRemoteConfig and SessionStore are wiki-server-
// only concerns anyway (auth/ never links into wiki-mcp).

// 32 random bytes, hex-encoded (64 chars) — via getrandom(2), not
// std::random_device (see PasswordHasher::randomSalt for why: blocks
// only if the kernel CSPRNG isn't seeded yet, otherwise as fast as a
// memcpy, no `/dev/urandom` fd-exhaustion risk).
std::string randomHexToken();

// SHA-256, hex-encoded.
std::string sha256Hex(const std::string& input);

}  // namespace wikicore::auth
