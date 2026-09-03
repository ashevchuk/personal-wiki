#pragma once

#include <string>
#include <string_view>

namespace wikicore::util {

// Renders a document body (markdown, GFM tables/strikethrough/tasklists
// enabled) to HTML. Raw HTML passthrough is deliberately disabled — see
// the .cpp — so this is safe to embed directly into a page without a
// separate sanitization pass. Throws std::runtime_error on (rare) parser
// failure.
std::string renderMarkdownToHtml(std::string_view markdown);

}  // namespace wikicore::util
