#include "vault/FolderService.h"

#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace wikicore::vault {

namespace {

std::string trimTrailingSlash(std::string p) {
  while (!p.empty() && p.back() == '/') p.pop_back();
  return p;
}

}  // namespace

int64_t FolderService::move(const std::string& oldRelativePathIn,
                             const std::string& newRelativePathIn) {
  const std::string oldRelativePath = trimTrailingSlash(oldRelativePathIn);
  const std::string newRelativePath = trimTrailingSlash(newRelativePathIn);

  if (oldRelativePath.empty() || newRelativePath.empty()) {
    throw InvalidFolderMoveError(
        "folder path must not be empty (refusing to move the vault root itself)");
  }
  if (newRelativePath == oldRelativePath) {
    throw InvalidFolderMoveError("source and destination are the same path");
  }
  if (newRelativePath.rfind(oldRelativePath + "/", 0) == 0) {
    throw InvalidFolderMoveError("can't move a folder into itself");
  }

  const fs::path oldAbs = vault_.pathGuard().resolve(oldRelativePath);
  if (!fs::exists(oldAbs) || !fs::is_directory(oldAbs)) {
    throw FolderNotFoundError(oldRelativePath);
  }

  const fs::path newAbs = vault_.pathGuard().resolve(newRelativePath);
  if (fs::exists(newAbs)) {
    throw FolderAlreadyExistsError(newRelativePath);
  }

  // Snapshot every currently-indexed document under the OLD prefix
  // BEFORE the move — this is what gets reconciled afterward.
  const std::string oldPrefix = oldRelativePath + "/";
  std::vector<std::string> affected;
  for (const auto& path : indexUpdater_.allIndexedPaths()) {
    if (path.rfind(oldPrefix, 0) == 0) affected.push_back(path);
  }

  fs::create_directories(newAbs.parent_path());
  std::error_code ec;
  fs::rename(oldAbs, newAbs, ec);
  if (ec) {
    throw fs::filesystem_error("failed to move folder", oldAbs, newAbs, ec);
  }

  int64_t reindexed = 0;
  const std::string newPrefix = newRelativePath + "/";
  for (const auto& oldPath : affected) {
    const std::string newPath = newPrefix + oldPath.substr(oldPrefix.size());
    indexUpdater_.removeOne(oldPath);
    if (indexBuilder_.reindexOneFile(newPath)) ++reindexed;
  }
  return reindexed;
}

bool FolderService::isEmpty(const std::string& relativePath) const {
  const fs::path abs = vault_.pathGuard().resolve(trimTrailingSlash(relativePath));
  if (!fs::exists(abs) || !fs::is_directory(abs)) {
    return false;
  }
  return fs::directory_iterator(abs) == fs::directory_iterator();
}

void FolderService::remove(const std::string& relativePathIn) {
  const std::string relativePath = trimTrailingSlash(relativePathIn);
  const fs::path abs = vault_.pathGuard().resolve(relativePath);
  if (!fs::exists(abs) || !fs::is_directory(abs)) {
    throw FolderNotFoundError(relativePath);
  }
  if (fs::directory_iterator(abs) != fs::directory_iterator()) {
    throw FolderNotEmptyError(relativePath);
  }
  std::error_code ec;
  fs::remove(abs, ec);
  if (ec) {
    throw fs::filesystem_error("failed to remove folder", abs, ec);
  }
}

}  // namespace wikicore::vault
