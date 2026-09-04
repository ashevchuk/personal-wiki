#include "vault/DocumentService.h"

#include "util/Excerpt.h"
#include "util/Time.h"
#include "util/Uuid.h"

#include <filesystem>

namespace wikicore::vault {

namespace {

std::string normalizeVisibility(const std::string& v) {
  return v == "public" ? "public" : "private";  // fail-safe, same rule as parsing
}

}  // namespace

DocumentRecord DocumentService::get(const std::string& relativePath) const {
  std::string raw;
  try {
    raw = vault_.readRaw(relativePath);
  } catch (const std::filesystem::filesystem_error&) {
    throw DocumentNotFoundError(relativePath);
  }
  const ParsedDocument parsed = parseFrontMatter(raw);
  return DocumentRecord{relativePath, parsed.frontMatter, parsed.body};
}

DocumentRecord DocumentService::create(const std::string& relativePath,
                                        const DocumentInput& input) {
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
  fm.created = now;
  fm.updated = now;

  return writeAndIndex(relativePath, input, std::move(fm));
}

DocumentRecord DocumentService::update(const std::string& relativePath,
                                        const DocumentInput& input) {
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
  if (const auto rowId = indexUpdater_.rowIdForPath(relativePath)) {
    snapshots_.record(*rowId, existingRaw);
  }

  FrontMatter fm;
  fm.id = existing.frontMatter.id.empty() ? util::newUuidV4() : existing.frontMatter.id;
  fm.title = input.title;
  fm.tags = input.tags;
  fm.visibility = normalizeVisibility(input.visibility);
  fm.type = input.type;
  fm.created = existing.frontMatter.created.empty() ? now : existing.frontMatter.created;
  fm.updated = now;

  return writeAndIndex(relativePath, input, std::move(fm));
}

void DocumentService::softDelete(const std::string& relativePath) {
  if (!vault_.exists(relativePath)) {
    throw DocumentNotFoundError(relativePath);
  }
  vault_.moveToTrash(relativePath);
  indexUpdater_.removeOne(relativePath);
}

DocumentRecord DocumentService::writeAndIndex(const std::string& relativePath,
                                               const DocumentInput& input,
                                               FrontMatter fm) {
  const std::string raw = serializeFrontMatter(fm, input.body);
  vault_.writeRawAtomic(relativePath, raw);
  const VaultRepository::FileStat stat = vault_.statFile(relativePath);

  index::DocumentIndexEntry entry;
  entry.uuid = fm.id;
  entry.path = relativePath;
  entry.title = fm.title;
  entry.docType = fm.type;
  entry.visibility = fm.visibility;
  entry.createdAt = fm.created;
  entry.updatedAt = fm.updated;
  entry.fileMtime = stat.mtimeUnix;
  entry.fileSize = stat.size;
  entry.excerpt = util::plainTextExcerpt(input.body);
  entry.tags = fm.tags;
  entry.body = input.body;
  indexUpdater_.upsertOne(entry);

  return DocumentRecord{relativePath, std::move(fm), input.body};
}

}  // namespace wikicore::vault
