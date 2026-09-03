#include "vault/VaultRepository.h"

#include <fstream>
#include <sstream>

namespace wikicore::vault {

std::string VaultRepository::readRaw(std::string_view relativePath) const {
  const std::filesystem::path fullPath = guard_.resolve(relativePath);

  std::ifstream file(fullPath, std::ios::binary);
  if (!file) {
    throw std::filesystem::filesystem_error(
        "failed to open document", fullPath,
        std::make_error_code(std::errc::no_such_file_or_directory));
  }

  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

}  // namespace wikicore::vault
