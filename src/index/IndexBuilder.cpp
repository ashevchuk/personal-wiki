#include "index/IndexBuilder.h"

#include "util/Excerpt.h"
#include "util/Uuid.h"
#include "vault/FrontMatter.h"
#include "vault/PathGuard.h"

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
  if (vault::isInsideAssetsFolder(relativePath)) return false;
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
  // is missing an id (the one exception is a copied id, below). But
  // documents.uuid is NOT NULL UNIQUE, so an empty
  // id still needs a stand-in value for the index row; this is generated
  // fresh (and NOT persisted), so it'll simply be regenerated differently
  // next time until the file is actually saved through DocumentService
  // once — acceptable churn for something that was never a stable
  // identifier to begin with.
  auto resolveUuid = [&]() {
    if (fm.id.empty()) return util::newUuidV4();
    if (!idHeldByAnotherFile(fm.id, relativePath)) return fm.id;
    const std::string freshId = util::newUuidV4();
    // If the file can't be rewritten (unusual front-matter layout, read-only
    // vault), the fresh id is a non-persisted stand-in like a missing id.
    if (const auto healed = vault::replaceFrontMatterId(raw, freshId)) {
      try {
        vault_.writeRawAtomic(relativePath, *healed);
      } catch (const std::exception&) {
      }
    }
    return freshId;
  };

  DocumentIndexEntry idxEntry;
  idxEntry.path = relativePath;
  idxEntry.title = fm.title;
  idxEntry.docType = fm.type;
  idxEntry.visibility = fm.visibility;
  idxEntry.createdAt = fm.created;
  idxEntry.updatedAt = fm.updated;
  idxEntry.dueAt = fm.due;
  idxEntry.recur = fm.recur;
  idxEntry.tags = fm.tags;
  idxEntry.body = parsed.body;
  idxEntry.excerpt = util::plainTextExcerpt(parsed.body);

  // A second attempt covers another connection indexing the same id for
  // another path between idHeldByAnotherFile's check and the upsert.
  constexpr int kAttempts = 2;
  for (int attempt = 1;; ++attempt) {
    idxEntry.uuid = resolveUuid();
    try {
      const auto stat = vault_.statFile(relativePath);
      idxEntry.fileMtime = stat.mtimeUnix;
      idxEntry.fileSize = stat.size;
    } catch (const std::exception&) {
      // leave at 0 -- not worth failing over
    }
    try {
      indexUpdater_.upsertOne(idxEntry);
      return true;
    } catch (const UuidTakenError&) {
      if (attempt == kAttempts) throw;
    }
  }
}

bool IndexBuilder::idHeldByAnotherFile(const std::string& id,
                                       const std::string& relativePath) {
  const auto holder = indexUpdater_.findPathByUuid(id);
  if (!holder || *holder == relativePath) return false;

  std::string holderRaw;
  try {
    holderRaw = vault_.readRaw(*holder);
  } catch (const std::exception&) {
    // Renamed outside the app: move the row so its history follows the file.
    if (indexUpdater_.rowIdForPath(relativePath)) {
      indexUpdater_.removeOne(*holder);
    } else {
      indexUpdater_.repathOne(*holder, relativePath);
    }
    return false;
  }
  if (vault::parseFrontMatter(holderRaw).frontMatter.id == id) return true;

  // The holder's file now carries another id and gets its own row updated
  // when it is re-indexed (the rescan visits it; the watcher has its event).
  indexUpdater_.releaseUuid(*holder);
  return false;
}

RescanStats IndexBuilder::fullRescan(RescanProgress* progress) {
  if (progress != nullptr) {
    progress->documentsIndexed = 0;
    progress->inProgress = true;
  }
  // Always clear inProgress on the way out, success or exception — a
  // rescan that throws partway through must not leave the progress
  // tracker permanently claiming "still running" forever.
  struct ProgressGuard {
    RescanProgress* p;
    ~ProgressGuard() {
      if (p != nullptr) p->inProgress = false;
    }
  } progressGuard{progress};

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
      if (isDotEntry(entry.path()) || entry.path().extension() == ".assets") {
        it.disable_recursion_pending();
      }
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
      if (progress != nullptr) progress->documentsIndexed = stats.documentsIndexed;
    }
  }

  for (const auto& indexedPath : indexUpdater_.allIndexedPaths()) {
    if (!seenPaths.count(indexedPath)) {
      indexUpdater_.removeOne(indexedPath);
      ++stats.staleRowsRemoved;
    }
  }

  // Wait for any embeddings queued by the upserts above. Ordinary Save
  // does not wait (that's how a long document avoids nginx 504); this
  // path is the explicit "reindex is done" contract.
  indexUpdater_.flushEmbeddings();
  return stats;
}

}  // namespace wikicore::index
