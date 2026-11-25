#pragma once

#include <string>
#include <string_view>

namespace wikicore::util {

// FIPS 180-4 SHA-256 of raw bytes, as 64 lowercase hex characters.
// Self-contained so wikicore (and wiki-mcp) need no OpenSSL; src/auth's
// OpenSSL-backed sha256Hex stays the one used by web-only code.
std::string sha256Hex(std::string_view data);

}  // namespace wikicore::util
