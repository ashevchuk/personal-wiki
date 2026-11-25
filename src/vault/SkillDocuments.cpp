#include "vault/SkillDocuments.h"

#include <cctype>

namespace wikicore::vault {

bool isSkillType(std::string_view type) {
  const auto isSpace = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
  while (!type.empty() && isSpace(type.front())) type.remove_prefix(1);
  while (!type.empty() && isSpace(type.back())) type.remove_suffix(1);
  if (type.size() != kSkillDocumentType.size()) return false;
  for (std::size_t i = 0; i < type.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(type[i])) != kSkillDocumentType[i]) return false;
  }
  return true;
}

void rejectSkillType(std::string_view type) {
  if (isSkillType(type)) throw SkillDocumentReadOnlyError();
}

}  // namespace wikicore::vault
