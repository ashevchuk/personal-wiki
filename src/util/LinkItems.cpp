#include "util/LinkItems.h"

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

bool isExternalUrl(std::string_view s) {
  return s.compare(0, 7, "http://") == 0 || s.compare(0, 8, "https://") == 0;
}

// Scans one line for `[label](url)` pairs, skipping `![alt](url)` image
// syntax entirely (that '!' marks an embedded asset, not a link to visit).
// No nested-bracket/escaping support -- same simplicity precedent as
// WikiLinks.cpp's "[[" / "]]" scan: find the next matching close, stop
// scanning at an unterminated one rather than guessing.
void scanLine(std::string_view line, int lineNo, std::vector<LinkItem>& out) {
  size_t pos = 0;
  while (true) {
    const size_t open = line.find('[', pos);
    if (open == std::string_view::npos) return;

    if (open > 0 && line[open - 1] == '!') {
      // Image syntax -- skip past its closing ')' if it has one,
      // otherwise just past this '[' so scanning can still find a REAL
      // link later on the same line.
      const size_t altClose = line.find(']', open + 1);
      if (altClose != std::string_view::npos && altClose + 1 < line.size() &&
          line[altClose + 1] == '(') {
        const size_t urlClose = line.find(')', altClose + 2);
        pos = urlClose == std::string_view::npos ? open + 1 : urlClose + 1;
      } else {
        pos = open + 1;
      }
      continue;
    }

    const size_t close = line.find(']', open + 1);
    if (close == std::string_view::npos) return;  // unterminated -- stop, don't guess

    if (close + 1 >= line.size() || line[close + 1] != '(') {
      pos = close + 1;
      continue;
    }

    const size_t urlClose = line.find(')', close + 2);
    if (urlClose == std::string_view::npos) return;

    const std::string label = trim(line.substr(open + 1, close - (open + 1)));
    const std::string url = trim(line.substr(close + 2, urlClose - (close + 2)));
    pos = urlClose + 1;

    if (label.empty() || url.empty() || !isExternalUrl(url)) continue;

    LinkItem item;
    item.url = url;
    item.label = label;
    item.lineNo = lineNo;
    out.push_back(std::move(item));
  }
}

}  // namespace

std::vector<LinkItem> extractExternalLinks(std::string_view markdown) {
  std::vector<LinkItem> items;
  size_t pos = 0;
  int lineNo = 0;
  while (pos <= markdown.size()) {
    ++lineNo;
    const size_t nl = markdown.find('\n', pos);
    const std::string_view line =
        nl == std::string_view::npos ? markdown.substr(pos) : markdown.substr(pos, nl - pos);

    scanLine(line, lineNo, items);

    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
  return items;
}

}  // namespace wikicore::util
