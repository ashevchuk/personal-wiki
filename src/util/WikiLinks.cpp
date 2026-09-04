#include "util/WikiLinks.h"

#include <algorithm>
#include <cctype>

namespace wikicore::util {

namespace {

std::string trim(std::string_view s) {
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
  return std::string(s.substr(start, end - start));
}

// See WikiLinks.h's doc comment for the exact normalization contract —
// this is the ONE place it's implemented; both public functions below
// route every target through it, so a target compares equal to a real
// document path regardless of which one produced it.
std::string normalizeTarget(std::string_view raw) {
  std::string t = trim(raw);
  if (!t.empty() && t.front() == '/') t.erase(t.begin());
  constexpr std::string_view kExt = ".md";
  const bool hasExt = t.size() >= kExt.size() &&
                       t.compare(t.size() - kExt.size(), kExt.size(), kExt) == 0;
  if (!hasExt) t += ".md";
  return t;
}

// Hand-rolled scan rather than std::regex — no other file in this
// codebase reaches for <regex> (PathGuard/FrontMatter/etc. all do plain
// string scanning too), and the grammar here is small and unambiguous
// enough not to need it: find "[[", find the next "]]", split whatever's
// between them on the first "|". Malformed input (an unclosed "[[") just
// stops scanning at that point rather than throwing — a wiki-link syntax
// slip in someone's own document body is not a rendering-time error.
struct RawLink {
  std::string target;  // normalized
  std::string label;   // raw target text (pre-normalization) unless a '|label' was given
};

std::vector<RawLink> scanRawLinks(std::string_view markdown) {
  std::vector<RawLink> links;
  size_t pos = 0;
  while (true) {
    const size_t open = markdown.find("[[", pos);
    if (open == std::string_view::npos) break;
    const size_t close = markdown.find("]]", open + 2);
    if (close == std::string_view::npos) break;  // unterminated -- stop, don't guess
    const std::string_view inner = markdown.substr(open + 2, close - (open + 2));
    pos = close + 2;

    const size_t bar = inner.find('|');
    std::string_view rawTarget = bar == std::string_view::npos ? inner : inner.substr(0, bar);
    std::string rawTargetTrimmed = trim(rawTarget);
    if (rawTargetTrimmed.empty()) continue;  // "[[]]" or "[[|label]]" -- nothing to link to

    RawLink link;
    link.target = normalizeTarget(rawTargetTrimmed);
    link.label = bar == std::string_view::npos ? rawTargetTrimmed : trim(inner.substr(bar + 1));
    if (link.label.empty()) link.label = rawTargetTrimmed;
    links.push_back(std::move(link));
  }
  return links;
}

}  // namespace

std::vector<std::string> extractWikiLinkTargets(std::string_view markdown) {
  std::vector<std::string> targets;
  for (auto& link : scanRawLinks(markdown)) {
    if (std::find(targets.begin(), targets.end(), link.target) == targets.end()) {
      targets.push_back(std::move(link.target));
    }
  }
  return targets;
}

std::string rewriteWikiLinksToMarkdownLinks(std::string_view markdown) {
  std::string out;
  out.reserve(markdown.size());
  size_t pos = 0;
  while (true) {
    const size_t open = markdown.find("[[", pos);
    if (open == std::string_view::npos) {
      out.append(markdown.substr(pos));
      break;
    }
    out.append(markdown.substr(pos, open - pos));

    const size_t close = markdown.find("]]", open + 2);
    if (close == std::string_view::npos) {
      // Unterminated "[[" -- emit the rest verbatim rather than eating it.
      out.append(markdown.substr(open));
      break;
    }
    const std::string_view inner = markdown.substr(open + 2, close - (open + 2));
    const size_t bar = inner.find('|');
    std::string_view rawTarget = bar == std::string_view::npos ? inner : inner.substr(0, bar);
    std::string rawTargetTrimmed = trim(rawTarget);

    if (rawTargetTrimmed.empty()) {
      // Nothing to link to -- pass the original "[[...]]" text through
      // unchanged rather than silently dropping it.
      out.append(markdown.substr(open, (close + 2) - open));
    } else {
      const std::string target = normalizeTarget(rawTargetTrimmed);
      std::string label = bar == std::string_view::npos ? rawTargetTrimmed : trim(inner.substr(bar + 1));
      if (label.empty()) label = rawTargetTrimmed;
      // A raw ']' or '(' inside the label/target would break CommonMark
      // link syntax -- neither is expected in a vault-relative path or a
      // short label, and this is a preprocessing pass over the AUTHOR'S
      // OWN document body (not untrusted external input), so it's left
      // as-is rather than escaped; md4c's own [[ ]] passthrough for a
      // malformed result degrades to "renders oddly", not a security
      // issue (MD_FLAG_NOHTMLSPANS/BLOCKS already strips any HTML either
      // way -- see MarkdownRenderer.cpp).
      out.append("[").append(label).append("](").append(target).append(")");
    }
    pos = close + 2;
  }
  return out;
}

}  // namespace wikicore::util
