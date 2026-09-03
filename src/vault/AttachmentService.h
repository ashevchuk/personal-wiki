#pragma once

#include "vault/VaultRepository.h"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace wikicore::vault {

class AttachmentRejectedError : public std::runtime_error {
 public:
  explicit AttachmentRejectedError(const std::string& reason)
      : std::runtime_error(reason) {}
};

struct AttachmentInfo {
  // Vault-relative path, e.g. "notes/foo.assets/diagram.png" — also a
  // valid relative markdown link target from "notes/foo.md".
  std::string relativePath;
  std::string mimeType;
  int64_t size = 0;
};

// Stores uploaded files in a co-located "<doc-stem>.assets/" folder next
// to their owning document, per the plan's storage layout. Filenames are
// sanitized (never trusted verbatim from the client) before ever reaching
// PathGuard, which still gets the final say.
class AttachmentService {
 public:
  explicit AttachmentService(VaultRepository& vault) : vault_(vault) {}

  // Throws AttachmentRejectedError if the extension isn't allowlisted or
  // `content` exceeds the size cap; DocumentNotFoundError-style callers
  // should check the owning document exists before calling this — it's
  // not re-checked here. Throws PathTraversalError if
  // `documentRelativePath` itself escapes the vault.
  AttachmentInfo store(const std::string& documentRelativePath,
                        const std::string& originalFilename,
                        const std::string& content);

 private:
  VaultRepository& vault_;
};

}  // namespace wikicore::vault
