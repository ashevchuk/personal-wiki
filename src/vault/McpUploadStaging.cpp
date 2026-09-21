#include "vault/McpUploadStaging.h"

#include "util/Uuid.h"
#include "vault/PathGuard.h"

#include <chrono>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace wikicore::vault {

namespace {

constexpr auto kMaxAge = std::chrono::hours(1);
constexpr std::string_view kDir = ".mcp-uploads";

bool isHex(char c) {
  return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

}  // namespace

bool McpUploadStaging::isValidId(std::string_view id) {
  if (id.size() != 36) return false;
  for (size_t i = 0; i < id.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (id[i] != '-') return false;
    } else if (!isHex(id[i])) {
      return false;
    }
  }
  return true;
}

std::string McpUploadStaging::metaRelative(std::string_view id) {
  return std::string(kDir) + "/" + std::string(id) + ".meta";
}

void McpUploadStaging::expireStale() {
  const fs::path dir = vault_.pathGuard().root() / std::string(kDir);
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return;
  const auto now = fs::file_time_type::clock::now();
  for (fs::directory_iterator it(dir, ec); !ec && it != fs::directory_iterator();
       it.increment(ec)) {
    if (!it->is_regular_file()) continue;
    const auto age = now - it->last_write_time();
    if (age <= kMaxAge) continue;
    const std::string name = it->path().filename().string();
    constexpr std::string_view kMetaExt = ".meta";
    if (name.size() > kMetaExt.size() &&
        name.compare(name.size() - kMetaExt.size(), kMetaExt.size(), kMetaExt) == 0) {
      const std::string id = name.substr(0, name.size() - kMetaExt.size());
      if (isValidId(id)) remove(id);
    }
  }
}

std::string McpUploadStaging::begin(const std::string& documentPath,
                                     const std::string& filename) {
  expireStale();
  const std::string id = util::newUuidV4();
  std::string meta;
  meta += documentPath;
  meta += '\n';
  meta += filename;
  meta += '\n';
  vault_.writeRawAtomic(metaRelative(id), meta);
  return id;
}

std::optional<McpUploadTicket> McpUploadStaging::get(std::string_view id) const {
  if (!isValidId(id)) return std::nullopt;
  std::string raw;
  try {
    const fs::path full = vault_.pathGuard().resolve(metaRelative(id));
    const auto age = fs::file_time_type::clock::now() - fs::last_write_time(full);
    if (age > kMaxAge) return std::nullopt;
    raw = vault_.readRaw(metaRelative(id));
  } catch (const std::exception&) {
    return std::nullopt;
  }
  std::istringstream in(raw);
  McpUploadTicket t;
  t.id = std::string(id);
  if (!std::getline(in, t.documentPath) || t.documentPath.empty()) return std::nullopt;
  if (!std::getline(in, t.filename) || t.filename.empty()) return std::nullopt;
  return t;
}

void McpUploadStaging::remove(std::string_view id) {
  if (!isValidId(id)) return;
  try {
    const fs::path full = vault_.pathGuard().resolve(metaRelative(id));
    std::error_code ec;
    fs::remove(full, ec);
  } catch (const PathTraversalError&) {
  }
}

}  // namespace wikicore::vault
