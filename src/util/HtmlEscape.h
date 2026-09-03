#pragma once

#include <string>
#include <string_view>

namespace wikicore::util {

// Escapes &, <, >, " for safe insertion into HTML text/attribute context.
// Use this on every untrusted string (document title, tags, filenames, ...)
// before it reaches a page — Drogon's CSP `[[key]]` interpolation does NOT
// escape anything itself (see docs/architecture.md), so this call is the
// only thing standing between user-entered text and HTML injection.
std::string escapeHtml(std::string_view in);

}  // namespace wikicore::util
