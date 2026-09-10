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

// Constant-time equality — use this instead of `==`/`std::string::compare`
// for anything comparing a caller-supplied credential (or its hash)
// against a stored one. `==` on std::string short-circuits at the first
// mismatched byte, which leaks how many leading bytes matched through
// response timing — a real, exploitable side channel for a comparison
// that gates authentication. Backed by OpenSSL's CRYPTO_memcmp rather
// than a hand-rolled XOR-accumulator loop specifically because a
// hand-rolled one is exactly the kind of thing an optimizing compiler
// can legally transform back into an early-exit `memcmp` — CRYPTO_memcmp
// is OpenSSL's own timing-safe primitive, not reinventing it here.
// Returns false immediately on a length mismatch WITHOUT calling
// CRYPTO_memcmp — safe, since every caller compares fixed-length hex
// digests, so length was never the secret being protected; only which
// byte differs is.
bool constantTimeEquals(const std::string& a, const std::string& b);

}  // namespace wikicore::auth
