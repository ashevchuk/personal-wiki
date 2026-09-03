#pragma once

#include "index/IndexUpdater.h"
#include "vault/FrontMatter.h"
#include "vault/VaultRepository.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace wikicore::vault {

class DocumentNotFoundError : public std::runtime_error {
 public:
  explicit DocumentNotFoundError(const std::string& path)
      : std::runtime_error("document not found: " + path) {}
};

class DocumentAlreadyExistsError : public std::runtime_error {
 public:
  explicit DocumentAlreadyExistsError(const std::string& path)
      : std::runtime_error("document already exists: " + path) {}
};

struct DocumentInput {
  std::string title;
  std::vector<std::string> tags;
  std::string visibility;  // "public" | "private"; anything else -> private
  std::string type;
  std::string body;
};

struct DocumentRecord {
  std::string path;
  FrontMatter frontMatter;
  std::string body;
};

// Orchestrates a document write: assemble front matter, serialize, write
// atomically through VaultRepository, then sync the SQLite index via
// IndexUpdater — in that order, so the index is never updated for content
// that failed to actually land on disk. Front matter fields not under the
// caller's control (id, created/updated timestamps) are managed here, not
// by the caller.
class DocumentService {
 public:
  DocumentService(VaultRepository& vault, index::IndexUpdater& indexUpdater)
      : vault_(vault), indexUpdater_(indexUpdater) {}

  // Throws DocumentAlreadyExistsError if `relativePath` is already
  // occupied, PathTraversalError if it escapes the vault. Assigns a fresh
  // UUID; created == updated == now.
  DocumentRecord create(const std::string& relativePath, const DocumentInput& input);

  // Throws DocumentNotFoundError if `relativePath` doesn't exist yet.
  // Preserves the existing id/created timestamp; sets updated = now.
  DocumentRecord update(const std::string& relativePath, const DocumentInput& input);

  // Reads + parses one document (front matter + body). Throws
  // DocumentNotFoundError if it doesn't exist.
  DocumentRecord get(const std::string& relativePath) const;

  // Moves the document to .trash/ and removes it from the index. Throws
  // DocumentNotFoundError if it doesn't exist.
  void softDelete(const std::string& relativePath);

 private:
  DocumentRecord writeAndIndex(const std::string& relativePath,
                                const DocumentInput& input, FrontMatter fm);

  VaultRepository& vault_;
  index::IndexUpdater& indexUpdater_;
};

}  // namespace wikicore::vault
