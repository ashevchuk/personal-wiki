#include "index/IndexBuilder.h"

#include "util/Excerpt.h"
#include "util/Uuid.h"
#include "vault/FrontMatter.h"

#include <filesystem>
#include <unordered_set>

namespace fs = std::filesystem;

namespace wikicore::index {

namespace {

bool isDotEntry(const fs::path& p) {
  const std::string name = p.filename().string();
  return !name.empty() && name[0] == '.';
}

}  // namespace

bool IndexBuilder::reindexOneFile(const std::string& relativePath) {
  std::string raw;
  try {
    raw = vault_.readRaw(relativePath);
  } catch (const std::exception&) {
    return false;  // missing, unreadable, or escapes the vault
  }

  const vault::ParsedDocument parsed = vault::parseFrontMatter(raw);
  vault::FrontMatter fm = parsed.frontMatter;
  if (fm.title.empty()) {
    fm.title = fs::path(relativePath).stem().string();
  }
  // A rescan describes what's on disk — it does NOT write anything back
  // to a file nobody asked it to touch, even if that file's front matter
  // is missing an id. But documents.uuid is NOT NULL UNIQUE, so an empty
  // id still needs a stand-in value for the index row; this is generated
  // fresh (and NOT persisted), so it'll simply be regenerated differently
  // next time until the file is actually saved through DocumentService
  // once — acceptable churn for something that was never a stable
  // identifier to begin with.
  const std::string uuidForIndex = fm.id.empty() ? util::newUuidV4() : fm.id;

  DocumentIndexEntry idxEntry;
  idxEntry.uuid = uuidForIndex;
  idxEntry.path = relativePath;
  idxEntry.title = fm.title;
  idxEntry.docType = fm.type;
  idxEntry.visibility = fm.visibility;
  idxEntry.createdAt = fm.created;
  idxEntry.updatedAt = fm.updated;
  idxEntry.tags = fm.tags;
  idxEntry.body = parsed.body;
  idxEntry.excerpt = util::plainTextExcerpt(parsed.body);
  try {
    const auto stat = vault_.statFile(relativePath);
    idxEntry.fileMtime = stat.mtimeUnix;
    idxEntry.fileSize = stat.size;
  } catch (const std::exception&) {
    // leave at 0 -- not worth failing over
  }

  indexUpdater_.upsertOne(idxEntry);
  return true;
}

RescanStats IndexBuilder::fullRescan() {
  RescanStats stats;
  const fs::path root = vault_.pathGuard().root();
  std::unordered_set<std::string> seenPaths;

  auto it = fs::recursive_directory_iterator(
      root, fs::directory_options::skip_permission_denied);
  const auto end = fs::recursive_directory_iterator();
  for (; it != end; ++it) {
    const fs::directory_entry& entry = *it;

    if (entry.is_directory()) {
      // Skip .git/.trash/anything-dot entirely — don't even descend, so a
      // huge .git history doesn't get walked for nothing.
      if (isDotEntry(entry.path())) it.disable_recursion_pending();
      continue;
    }
    if (!entry.is_regular_file() || entry.path().extension() != ".md" ||
        isDotEntry(entry.path())) {
      continue;
    }

    const std::string relativePath =
        fs::relative(entry.path(), root).generic_string();
    if (reindexOneFile(relativePath)) {
      seenPaths.insert(relativePath);
      ++stats.documentsIndexed;
    }
  }

  for (const auto& indexedPath : indexUpdater_.allIndexedPaths()) {
    if (!seenPaths.count(indexedPath)) {
      indexUpdater_.removeOne(indexedPath);
      ++stats.staleRowsRemoved;
    }
  }

  return stats;
}

}  // namespace wikicore::index
