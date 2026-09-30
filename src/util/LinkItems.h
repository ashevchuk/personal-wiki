#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace wikicore::util {

// One external `[label](https://...)` markdown link found in a document's
// body. Deliberately EXTERNAL only (http/https) -- an internal
// `[[wiki-link]]` uses different syntax entirely (see WikiLinks.h) and
// never matches this, and a relative markdown link (`[x](assets/foo.png)`,
// `[x](d/other-doc.md)`) is filtered out on purpose: those already have
// their own dedicated mechanisms (attachments, wiki-links/backlinks), and
// counting them here too would mean the same relationship shows up in two
// unrelated aggregations. This is specifically for "a bookmark embedded
// in a longer document" -- a curated list/article that links OUT to the
// web, one entry per link.
struct LinkItem {
  std::string url;
  std::string label;
  int lineNo = 0;
};

// Every external markdown link in `markdown`, in document order. Hand-
// rolled scan, not <regex> -- same reasoning as WikiLinks.cpp/TodoItems.cpp:
// the document author's own body text, a small unambiguous grammar. Only
// single-line links are recognized (label and URL on the same line) --
// CommonMark technically allows a link to span lines, but every realistic
// "site + one-line description" list writes it on one line, and scanning
// line-by-line keeps this exactly as simple as the other two extractors
// in this directory.
std::vector<LinkItem> extractExternalLinks(std::string_view markdown);

}  // namespace wikicore::util
