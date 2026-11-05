#include "util/Excerpt.h"

#include <algorithm>

namespace wikicore::util {

std::string plainTextExcerpt(const std::string& body, size_t maxLen) {
  std::string flat;
  flat.reserve(std::min(body.size(), maxLen + 1));
  bool lastWasSpace = false;
  for (char c : body) {
    const bool isSpace = (c == '\n' || c == '\r' || c == '\t' || c == ' ');
    if (isSpace) {
      if (!lastWasSpace && !flat.empty()) flat += ' ';
      lastWasSpace = true;
    } else {
      flat += c;
      lastWasSpace = false;
    }
    if (flat.size() >= maxLen) break;
  }
  const bool truncated = flat.size() >= maxLen;
  if (truncated) {
    // The cut above is by byte, so it can land inside a multi-byte UTF-8
    // character (Cyrillic is 2 bytes each). Drop the incomplete tail:
    // an invalid sequence stored here later makes nlohmann::json refuse to
    // serialize any search result carrying this excerpt.
    size_t lead = flat.size();
    while (lead > 0 && (static_cast<unsigned char>(flat[lead - 1]) & 0xC0) == 0x80) --lead;
    if (lead > 0) {
      const unsigned char first = static_cast<unsigned char>(flat[lead - 1]);
      const size_t expected = first >= 0xF0 ? 4 : first >= 0xE0 ? 3 : first >= 0xC0 ? 2 : 1;
      if (flat.size() - (lead - 1) < expected) flat.resize(lead - 1);
    }
  }
  while (!flat.empty() && flat.back() == ' ') flat.pop_back();
  if (truncated) flat += "...";
  return flat;
}

}  // namespace wikicore::util
