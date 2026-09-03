#include "vault/DocumentService.h"

#include "util/Time.h"
#include "util/Uuid.h"

#include <filesystem>

namespace wikicore::vault {

namespace {

std::string normalizeVisibility(const std::string& v) {
  return v == "public" ? "public" : "private";  // fail-safe, same rule as parsing
}

// Short plain-text preview for search results/listings — collapse
// whitespace, hard-truncate. Not markdown-aware (M3's search UI can do
// better later); good enough to show something readable in a list.
std::string makeExcerpt(const std::string& body, size_t maxLen = 240) {
  std::string flat;
  flat.reserve(std::min(body.size(), maxLen + 1));
  bool lastWasSpace = false;
  for (char c : body) {
    const bool isSpace = (c == '\n' || c == '\r' || c == '\t' || c == ' ');
    if (isSpace) {
      if (!lastWasSpace && !flat.empty()) flat += ' ';
      lastWasSpace = true;
    } else {
      flat += c;
      lastWasSpace = false;
    }
    if (flat.size() >= maxLen) break;
  }
  while (!flat.empty() && flat.back() == ' ') flat.pop_back();
  if (flat.size() >= maxLen) flat += "...";
  return flat;
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

  const ParsedDocument existing = parseFrontMatter(vault_.readRaw(relativePath));
  const std::string now = util::nowIso8601();

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
  entry.excerpt = makeExcerpt(input.body);
  entry.tags = fm.tags;
  entry.body = input.body;
  indexUpdater_.upsertOne(entry);

  return DocumentRecord{relativePath, std::move(fm), input.body};
}

}  // namespace wikicore::vault
