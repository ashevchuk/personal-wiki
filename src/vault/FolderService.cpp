#include "vault/FolderService.h"

#include "util/WikiLinks.h"
#include "vault/FrontMatter.h"
#include "vault/PathGuard.h"

#include <filesystem>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace wikicore::vault {

namespace {

std::string trimTrailingSlash(std::string p) {
  while (!p.empty() && p.back() == '/') p.pop_back();
  return p;
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
  if (from.empty() || from == to) return;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
}

// App-inserted attachment hrefs (`assets/{folder}/…`) and vault-relative
// markdown hrefs (`]({folder}/…)`). Stem-relative `](foo.assets/` is
// unchanged on a folder move — the stem didn't change, only the parent
// path, so a same-directory relative link still resolves.
std::string rewriteAssetPrefix(std::string body, const std::string& oldPrefix,
                               const std::string& newPrefix) {
  if (oldPrefix.empty() || oldPrefix == newPrefix) return body;
  replaceAll(body, "assets/" + oldPrefix, "assets/" + newPrefix);
  replaceAll(body, "](" + oldPrefix, "](" + newPrefix);
  return body;
}

bool isDotEntry(const fs::path& p) {
  const std::string name = p.filename().string();
  return !name.empty() && name[0] == '.';
}

// Wiki folders are implicit in document paths — the browse UI only lists
// .md files (via /api/nav/tree). Loose leftovers (MCP upload probes,
// orphaned .tap/.bin) are invisible there, so "empty" for delete means
// "no markdown documents", not "no directory entries at all". Found
// live: mcp_upload_probe showed "(none)" documents and still 409'd
// because of leftover probe files.
bool containsMarkdownDocument(const fs::path& dir) {
  std::error_code ec;
  auto it = fs::recursive_directory_iterator(
      dir, fs::directory_options::skip_permission_denied, ec);
  const auto end = fs::recursive_directory_iterator();
  for (; !ec && it != end; ++it) {
    if (it->is_directory()) {
      if (isDotEntry(it->path())) it.disable_recursion_pending();
      continue;
    }
    if (it->is_regular_file() && it->path().extension() == ".md" &&
        !isDotEntry(it->path())) {
      return true;
    }
  }
  return false;
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
  // BEFORE the move — this is what gets re-pathed afterward.
  const std::string oldPrefix = oldRelativePath + "/";
  const std::string newPrefix = newRelativePath + "/";
  std::vector<std::string> affected;
  const auto indexedBefore = indexUpdater_.allIndexedPaths();
  for (const auto& path : indexedBefore) {
    if (path.rfind(oldPrefix, 0) == 0) affected.push_back(path);
  }

  fs::create_directories(newAbs.parent_path());
  std::error_code ec;
  fs::rename(oldAbs, newAbs, ec);
  if (ec) {
    throw fs::filesystem_error("failed to move folder", oldAbs, newAbs, ec);
  }

  // Rewrite bodies on disk at their POST-move paths, before the index
  // catches up. get()/readRaw of an affected document's OLD path would
  // 404 — the file already lives under newPrefix.
  std::unordered_set<std::string> rewrittenDiskPaths;
  for (const auto& path : indexedBefore) {
    const bool moved = path.rfind(oldPrefix, 0) == 0;
    const std::string diskPath =
        moved ? newPrefix + path.substr(oldPrefix.size()) : path;
    std::string raw;
    try {
      raw = vault_.readRaw(diskPath);
    } catch (const PathTraversalError&) {
      continue;
    } catch (const std::filesystem::filesystem_error&) {
      continue;
    }

    const ParsedDocument parsed = parseFrontMatter(raw);
    std::string rewritten =
        util::rewriteWikiLinkTargetPrefix(parsed.body, oldPrefix, newPrefix);
    rewritten = rewriteAssetPrefix(std::move(rewritten), oldPrefix, newPrefix);
    if (rewritten == parsed.body) continue;

    vault_.writeRawAtomic(diskPath, serializeFrontMatter(parsed.frontMatter, rewritten));
    rewrittenDiskPaths.insert(diskPath);
  }

  int64_t reindexed = 0;
  for (const auto& oldPath : affected) {
    const std::string newPath = newPrefix + oldPath.substr(oldPrefix.size());
    indexUpdater_.repathOne(oldPath, newPath);
    if (indexBuilder_.reindexOneFile(newPath)) ++reindexed;
  }

  // Outsiders whose bodies changed still sit at the same index path —
  // reindex from the rewritten file so document_links / FTS catch up.
  // Moved docs were already reindexOneFile'd above.
  for (const auto& diskPath : rewrittenDiskPaths) {
    if (diskPath.rfind(newPrefix, 0) == 0) continue;
    indexBuilder_.reindexOneFile(diskPath);
  }

  return reindexed;
}

bool FolderService::isEmpty(const std::string& relativePath) const {
  const fs::path abs = vault_.pathGuard().resolve(trimTrailingSlash(relativePath));
  if (!fs::exists(abs) || !fs::is_directory(abs)) {
    return false;
  }
  return !containsMarkdownDocument(abs);
}

void FolderService::remove(const std::string& relativePathIn) {
  const std::string relativePath = trimTrailingSlash(relativePathIn);
  const fs::path abs = vault_.pathGuard().resolve(relativePath);
  if (!fs::exists(abs) || !fs::is_directory(abs)) {
    throw FolderNotFoundError(relativePath);
  }
  if (containsMarkdownDocument(abs)) {
    throw FolderNotEmptyError(relativePath);
  }
  std::error_code ec;
  fs::remove_all(abs, ec);
  if (ec) {
    throw fs::filesystem_error("failed to remove folder", abs, ec);
  }
}

}  // namespace wikicore::vault
