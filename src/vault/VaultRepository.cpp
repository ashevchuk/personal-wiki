#include "vault/VaultRepository.h"

#include "util/Uuid.h"

#include <chrono>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace wikicore::vault {

std::string VaultRepository::readRaw(std::string_view relativePath) const {
  const fs::path fullPath = guard_.resolve(relativePath);

  std::ifstream file(fullPath, std::ios::binary);
  if (!file) {
    throw fs::filesystem_error(
        "failed to open document", fullPath,
        std::make_error_code(std::errc::no_such_file_or_directory));
  }

  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

bool VaultRepository::exists(std::string_view relativePath) const {
  try {
    return fs::exists(guard_.resolve(relativePath));
  } catch (const PathTraversalError&) {
    return false;
  }
}

void VaultRepository::writeRawAtomic(std::string_view relativePath,
                                      std::string_view content) const {
  const fs::path fullPath = guard_.resolve(relativePath);
  fs::create_directories(fullPath.parent_path());

  const fs::path tempPath = fullPath.parent_path() /
      (fullPath.filename().string() + ".tmp-" + util::newUuidV4());
  {
    std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
    if (!file) {
      throw fs::filesystem_error(
          "failed to open temp file for atomic write", tempPath,
          std::make_error_code(std::errc::io_error));
    }
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!file) {
      std::error_code ignored;
      fs::remove(tempPath, ignored);
      throw fs::filesystem_error("failed to write temp file", tempPath,
                                  std::make_error_code(std::errc::io_error));
    }
  }

  std::error_code ec;
  fs::rename(tempPath, fullPath, ec);
  if (ec) {
    std::error_code ignored;
    fs::remove(tempPath, ignored);
    throw fs::filesystem_error("failed to atomically replace document",
                                tempPath, fullPath, ec);
  }
}

void VaultRepository::copyFileAtomic(std::string_view relativePath,
                                      const fs::path& source) const {
  const fs::path fullPath = guard_.resolve(relativePath);
  fs::create_directories(fullPath.parent_path());

  const fs::path tempPath = fullPath.parent_path() /
      (fullPath.filename().string() + ".tmp-" + util::newUuidV4());
  std::error_code copyEc;
  fs::copy_file(source, tempPath, fs::copy_options::none, copyEc);
  if (copyEc) {
    std::error_code ignored;
    fs::remove(tempPath, ignored);
    throw fs::filesystem_error("failed to copy into temp file for atomic write",
                                source, tempPath, copyEc);
  }

  std::error_code ec;
  fs::rename(tempPath, fullPath, ec);
  if (ec) {
    std::error_code ignored;
    fs::remove(tempPath, ignored);
    throw fs::filesystem_error("failed to atomically replace document",
                                tempPath, fullPath, ec);
  }
}

void VaultRepository::moveToTrash(std::string_view relativePath) const {
  const fs::path source = guard_.resolve(relativePath);
  if (!fs::exists(source)) {
    throw fs::filesystem_error(
        "document not found", source,
        std::make_error_code(std::errc::no_such_file_or_directory));
  }

  const fs::path trashRelative = fs::path(".trash") / fs::path(relativePath);
  const fs::path dest = guard_.resolve(trashRelative.generic_string());
  fs::create_directories(dest.parent_path());

  std::error_code ec;
  fs::rename(source, dest, ec);
  if (ec) {
    throw fs::filesystem_error("failed to move document to trash", source,
                                dest, ec);
  }

  const fs::path sourceAssets =
      source.parent_path() / (source.stem().string() + ".assets");
  if (fs::exists(sourceAssets)) {
    const fs::path destAssets =
        dest.parent_path() / (dest.stem().string() + ".assets");
    std::error_code assetsEc;
    fs::rename(sourceAssets, destAssets, assetsEc);
    if (assetsEc) {
      // The document itself is already trashed at this point — surface
      // the failure rather than silently leaving orphaned assets behind,
      // but don't try to roll the document move back over it.
      throw fs::filesystem_error(
          "moved document but failed to move its assets folder",
          sourceAssets, destAssets, assetsEc);
    }
  }
}

void VaultRepository::renameDocument(std::string_view oldRelativePath,
                                     std::string_view newRelativePath) const {
  const fs::path source = guard_.resolve(oldRelativePath);
  if (!fs::exists(source)) {
    throw fs::filesystem_error(
        "document not found", source,
        std::make_error_code(std::errc::no_such_file_or_directory));
  }

  const fs::path dest = guard_.resolve(newRelativePath);
  fs::create_directories(dest.parent_path());

  std::error_code ec;
  fs::rename(source, dest, ec);
  if (ec) {
    throw fs::filesystem_error("failed to rename document", source, dest, ec);
  }

  const fs::path sourceAssets =
      source.parent_path() / (source.stem().string() + ".assets");
  if (fs::exists(sourceAssets)) {
    const fs::path destAssets =
        dest.parent_path() / (dest.stem().string() + ".assets");
    std::error_code assetsEc;
    fs::rename(sourceAssets, destAssets, assetsEc);
    if (assetsEc) {
      throw fs::filesystem_error(
          "moved document but failed to move its assets folder",
          sourceAssets, destAssets, assetsEc);
    }
  }
}

VaultRepository::FileStat VaultRepository::statFile(
    std::string_view relativePath) const {
  const fs::path fullPath = guard_.resolve(relativePath);

  FileStat stat;
  stat.size = static_cast<int64_t>(fs::file_size(fullPath));

  // Portable file_time_type -> unix-time conversion (pre-clock_cast
  // trick): rebase the file clock's epoch onto system_clock's "now".
  const auto fileTime = fs::last_write_time(fullPath);
  const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      fileTime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
  stat.mtimeUnix = std::chrono::system_clock::to_time_t(systemTime);

  return stat;
}

std::vector<std::string> VaultRepository::listRegularFiles(
    std::string_view relativeDir) const {
  const fs::path fullPath = guard_.resolve(relativeDir);
  std::error_code ec;
  if (!fs::is_directory(fullPath, ec)) return {};

  std::vector<std::string> out;
  for (fs::directory_iterator it(fullPath, ec); !ec && it != fs::directory_iterator();
       it.increment(ec)) {
    if (!it->is_regular_file()) continue;
    const std::string name = it->path().filename().string();
    if (name.empty() || name.front() == '.') continue;
    const fs::path rel = fs::path(std::string(relativeDir)) / name;
    out.push_back(rel.generic_string());
  }
  return out;
}

void VaultRepository::removeFile(std::string_view relativePath) const {
  const fs::path fullPath = guard_.resolve(relativePath);
  std::error_code ec;
  fs::remove(fullPath, ec);
}

}  // namespace wikicore::vault
