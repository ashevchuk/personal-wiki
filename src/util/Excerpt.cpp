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
  while (!flat.empty() && flat.back() == ' ') flat.pop_back();
  if (flat.size() >= maxLen) flat += "...";
  return flat;
}

}  // namespace wikicore::util
