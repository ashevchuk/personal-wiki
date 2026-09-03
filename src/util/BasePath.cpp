#include "util/BasePath.h"

namespace wikicore::util {

std::string normalizeBasePath(std::string_view raw) {
  std::string s(raw);
  while (!s.empty() && s.back() == '/') s.pop_back();
  if (s.empty()) return "";
  if (s.front() != '/') s = "/" + s;
  return s;
}

std::string withBasePath(const std::string& basePath, std::string_view path) {
  return basePath + std::string(path);
}

}  // namespace wikicore::util
