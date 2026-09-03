#pragma once

#include <cstddef>
#include <string>

namespace wikicore::util {

// Short plain-text preview for search results/listings — collapses
// whitespace, hard-truncates at maxLen. Not markdown-aware (stripping
// syntax cleanly is more than this needs); good enough to show something
// readable in a list. Shared between DocumentService (index entry at
// save time) and IndexBuilder (full rescan) so both populate
// documents.excerpt the same way.
std::string plainTextExcerpt(const std::string& body, size_t maxLen = 240);

}  // namespace wikicore::util
