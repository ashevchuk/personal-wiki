#include "vault/DocumentService.h"

#include "util/Excerpt.h"
#include "util/Time.h"
#include "util/Uuid.h"
#include "util/WikiLinks.h"
#include "vault/PathGuard.h"

#include <filesystem>

namespace wikicore::vault {

namespace fs = std::filesystem;

namespace {

constexpr int kUpsertAttempts = 2;

std::string normalizeVisibility(const std::string& v) {
  return v == "public" ? "public" : "private";  // fail-safe, same rule as parsing
}

// Trailing slashes stripped, then a missing ".md" (any case) is made
// into lowercase ".md". Empty after stripping stays empty — the caller
// rejects that rather than producing a document named ".md".
std::string ensureMarkdownExtension(std::string path) {
  while (!path.empty() && path.back() == '/') path.pop_back();
  if (path.empty()) return path;
  if (path.size() >= 3) {
    const unsigned char dot = static_cast<unsigned char>(path[path.size() - 3]);
    const unsigned char m = static_cast<unsigned char>(path[path.size() - 2]);
    const unsigned char d = static_cast<unsigned char>(path[path.size() - 1]);
    if (dot == '.' && (m == 'm' || m == 'M') && (d == 'd' || d == 'D')) {
      path.replace(path.size() - 3, 3, ".md");
      return path;
    }
  }
  return path + ".md";
}

std::string assetsDirFor(const std::string& docPath) {
  const fs::path p(docPath);
  return (p.parent_path() / (p.stem().string() + ".assets")).generic_string();
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
  if (from.empty() || from == to) return;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
}

// App-inserted attachment hrefs are `assets/{vault-rel}/file` relative
// to <base>. Vault-relative markdown hrefs `](notes/foo.assets/…)` and,
// in the renamed document only, same-directory `](foo.assets/…)` are
// rewritten too. Stem-relative replacement is NOT applied globally —
// `](foo.assets/x)` in another folder means that folder's own assets.
std::string rewriteAssetHrefs(std::string body, const std::string& oldPath,
                              const std::string& newPath, bool isRenamedDoc) {
  const std::string oldAssets = assetsDirFor(oldPath);
  const std::string newAssets = assetsDirFor(newPath);
  if (oldAssets != newAssets) {
    replaceAll(body, "assets/" + oldAssets + "/", "assets/" + newAssets + "/");
    replaceAll(body, oldAssets + "/", newAssets + "/");
    if (isRenamedDoc) {
      const std::string oldStem = fs::path(oldPath).stem().string();
      const std::string newStem = fs::path(newPath).stem().string();
      if (oldStem != newStem) {
        replaceAll(body, "](" + oldStem + ".assets/", "](" + newStem + ".assets/");
      }
    }
  }
  return body;
}

}  // namespace

DocumentRecord DocumentService::get(const std::string& relativePath) const {
  requireDocumentPath(relativePath);
  std::string raw;
  try {
    raw = vault_.readRaw(relativePath);
  } catch (const std::filesystem::filesystem_error&) {
    throw DocumentNotFoundError(relativePath);
  }
  const ParsedDocument parsed = parseFrontMatter(raw);
  return DocumentRecord{relativePath, parsed.frontMatter, parsed.body};
}

DocumentRecord DocumentService::create(const std::string& relativePathIn,
                                        const DocumentInput& input) {
  const std::string relativePath = ensureMarkdownExtension(relativePathIn);
  if (relativePath.empty()) {
    throw PathTraversalError(relativePathIn);
  }
  requireDocumentPath(relativePath);
  if (vault_.exists(relativePath)) {
    throw DocumentAlreadyExistsError(relativePath);
  }

  const std::string now = util::nowIso8601();
  FrontMatter fm;
  fm.id = util::newUuidV4();
  fm.title = input.title;
  fm.tags = input.tags;
  fm.visibility = normalizeVisibility(input.visibility);
  fm.type = input.type;
  fm.description = input.description.value_or("");
  fm.due = input.due;
  fm.recur = input.recur;
  fm.created = now;
  fm.updated = now;

  return writeAndIndex(relativePath, input, std::move(fm));
}

DocumentRecord DocumentService::update(const std::string& relativePath,
                                        const DocumentInput& input) {
  requireDocumentPath(relativePath);
  if (!vault_.exists(relativePath)) {
    throw DocumentNotFoundError(relativePath);
  }

  const std::string existingRaw = vault_.readRaw(relativePath);
  const ParsedDocument existing = parseFrontMatter(existingRaw);
  const std::string now = util::nowIso8601();

  // Snapshot the PRE-edit state before it gets overwritten below — see
  // SnapshotStore's own doc comment for the exact semantics (a snapshot
  // per past state, current content never duplicated into the table).
  // Looked up by path rather than threaded through as a parameter: this
  // document is guaranteed already indexed (vault_.exists just returned
  // true for it, and every write goes through writeAndIndex, which
  // always calls indexUpdater_.upsertOne) — a missing rowid here would
  // mean the file and the index have already drifted apart, a
  // pre-existing inconsistency this call isn't responsible for masking.
  // Only when the body actually changed — history.js's diff view only
  // ever compares body text, so a front-matter-only edit (e.g. flipping
  // visibility) would otherwise leave a snapshot row nothing can show
  // a difference for.
  if (existing.body != input.body) {
    if (const auto rowId = indexUpdater_.rowIdForPath(relativePath)) {
      snapshots_.record(*rowId, existingRaw);
    }
  }

  FrontMatter fm;
  fm.id = idToWrite(existing.frontMatter.id, relativePath);
  fm.title = input.title;
  fm.tags = input.tags;
  fm.visibility = normalizeVisibility(input.visibility);
  fm.type = input.type;
  fm.description = input.description.value_or(existing.frontMatter.description);
  fm.due = input.due;
  fm.recur = input.recur;
  fm.created = existing.frontMatter.created.empty() ? now : existing.frontMatter.created;
  fm.updated = now;

  return writeAndIndex(relativePath, input, std::move(fm), existingRaw);
}

DocumentRecord DocumentService::rename(const std::string& oldRelativePathIn,
                                       const std::string& newRelativePathIn) {
  const std::string oldPath = ensureMarkdownExtension(oldRelativePathIn);
  const std::string newPath = ensureMarkdownExtension(newRelativePathIn);
  if (oldPath.empty() || newPath.empty()) {
    throw InvalidDocumentMoveError("document path must not be empty");
  }
  requireDocumentPath(oldPath);
  requireDocumentPath(newPath);
  if (oldPath == newPath) {
    return get(oldPath);
  }
  if (!vault_.exists(oldPath)) {
    throw DocumentNotFoundError(oldPath);
  }
  if (vault_.exists(newPath)) {
    throw DocumentAlreadyExistsError(newPath);
  }

  // PathGuard both sides before touching the filesystem — renameDocument
  // does too, but a traversal in `newPath` must fail before the source
  // file is moved.
  (void)vault_.pathGuard().resolve(oldPath);
  (void)vault_.pathGuard().resolve(newPath);

  vault_.renameDocument(oldPath, newPath);
  indexUpdater_.repathOne(oldPath, newPath);

  // Best-effort body rewrite + reindex of every currently-indexed
  // document (including the renamed one, now at newPath). The filesystem
  // move already succeeded; a parse failure on one inbound linker is
  // left for the next `--reindex`, same fallback as FolderService::move.
  const auto indexed = indexUpdater_.allIndexedPaths();
  for (const auto& path : indexed) {
    DocumentRecord rec;
    std::string existingRaw;
    try {
      rec = get(path);
      existingRaw = vault_.readRaw(path);
    } catch (const DocumentNotFoundError&) {
      continue;
    } catch (const std::filesystem::filesystem_error&) {
      continue;
    } catch (const PathTraversalError&) {
      continue;
    }

    const bool self = (path == newPath);
    std::string rewritten =
        util::rewriteWikiLinkTargets(rec.body, oldPath, newPath);
    rewritten = rewriteAssetHrefs(std::move(rewritten), oldPath, newPath, self);
    if (rewritten == rec.body) {
      if (self) {
        // Path column already updated by repathOne; FTS/links don't
        // store this document's own path. Nothing else to write.
      }
      continue;
    }

    // Only the body is rewritten: this document was not the one renamed, and
    // its front matter may hold keys and comments FrontMatter does not model.
    std::string raw = replaceBody(existingRaw, rewritten);
    const std::string id = idToWrite(rec.frontMatter.id, path);
    if (id != rec.frontMatter.id) {
      // Without a rewritable block the new id is a non-persisted stand-in.
      if (auto withId = withFrontMatterId(raw, id)) raw = std::move(*withId);
      rec.frontMatter.id = id;
    }
    vault_.writeRawAtomic(path, raw);

    index::DocumentIndexEntry entry;
    entry.uuid = rec.frontMatter.id;
    entry.path = path;
    entry.title = rec.frontMatter.title;
    entry.docType = rec.frontMatter.type;
    entry.visibility = rec.frontMatter.visibility;
    entry.createdAt = rec.frontMatter.created;
    entry.updatedAt = rec.frontMatter.updated;
    entry.dueAt = rec.frontMatter.due;
    entry.recur = rec.frontMatter.recur;
    entry.tags = rec.frontMatter.tags;
    entry.body = rewritten;
    entry.excerpt = util::plainTextExcerpt(rewritten);
    try {
      const auto stat = vault_.statFile(path);
      entry.fileMtime = stat.mtimeUnix;
      entry.fileSize = stat.size;
    } catch (const std::exception&) {
    }
    try {
      indexUpdater_.upsertOne(entry);
    } catch (const index::UuidTakenError&) {
      // Same race as in writeAndIndex: take a fresh id and retry once.
      entry.uuid = util::newUuidV4();
      if (auto withId = withFrontMatterId(raw, entry.uuid)) {
        vault_.writeRawAtomic(path, *withId);
      }
      indexUpdater_.upsertOne(entry);
    }
  }

  return get(newPath);
}

std::string DocumentService::idToWrite(const std::string& existingId,
                                       const std::string& relativePath) const {
  if (existingId.empty()) return util::newUuidV4();
  const auto holder = indexUpdater_.findPathByUuid(existingId);
  return holder && *holder != relativePath ? util::newUuidV4() : existingId;
}

void DocumentService::softDelete(const std::string& relativePath) {
  requireDocumentPath(relativePath);
  if (!vault_.exists(relativePath)) {
    throw DocumentNotFoundError(relativePath);
  }
  vault_.moveToTrash(relativePath);
  indexUpdater_.removeOne(relativePath);
}

DocumentRecord DocumentService::writeAndIndex(const std::string& relativePath,
                                               const DocumentInput& input,
                                               FrontMatter fm,
                                               std::string_view existingRaw) {
  index::DocumentIndexEntry entry;
  entry.path = relativePath;
  entry.title = fm.title;
  entry.docType = fm.type;
  entry.visibility = fm.visibility;
  entry.createdAt = fm.created;
  entry.updatedAt = fm.updated;
  entry.dueAt = fm.due;
  entry.recur = fm.recur;
  entry.excerpt = util::plainTextExcerpt(input.body);
  entry.tags = fm.tags;
  entry.body = input.body;

  // A second attempt covers another connection (VaultWatcher) indexing this
  // id for another path after idToWrite's check.
  for (int attempt = 1;; ++attempt) {
    vault_.writeRawAtomic(relativePath, serializeFrontMatter(fm, input.body, existingRaw));
    const VaultRepository::FileStat stat = vault_.statFile(relativePath);
    entry.uuid = fm.id;
    entry.fileMtime = stat.mtimeUnix;
    entry.fileSize = stat.size;
    try {
      indexUpdater_.upsertOne(entry);
      break;
    } catch (const index::UuidTakenError&) {
      if (attempt == kUpsertAttempts) throw;
      fm.id = util::newUuidV4();
    }
  }

  return DocumentRecord{relativePath, std::move(fm), input.body};
}

}  // namespace wikicore::vault
