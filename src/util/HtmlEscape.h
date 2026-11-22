#pragma once

#include <string>
#include <string_view>

namespace wikicore::util {

// Escapes &, <, >, " for safe insertion into HTML text/attribute context.
// Use this on every untrusted string (document title, tags, filenames, ...)
// before it is inserted into server-built HTML.
std::string escapeHtml(std::string_view in);

}  // namespace wikicore::util
