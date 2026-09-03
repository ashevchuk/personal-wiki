#pragma once

#include "vault/PathGuard.h"

#include <string>
#include <string_view>

namespace wikicore::vault {

// The read/write surface over the vault filesystem. Everything here goes
// through PathGuard — nothing in this class (or its callers) is allowed
// to build a vault path by any other means. M1 ships read-only; write
// (atomic save, attachments) lands M2 as DocumentService/AttachmentService.
class VaultRepository {
 public:
  explicit VaultRepository(std::filesystem::path vaultRoot)
      : guard_(std::move(vaultRoot)) {}

  // Reads the full raw content (front matter + body, unparsed) of the
  // document at `relativePath`. Throws PathTraversalError if the path
  // escapes the vault, or std::filesystem::filesystem_error /
  // std::ios_base::failure if it can't be read (missing, a directory,
  // permissions, ...).
  [[nodiscard]] std::string readRaw(std::string_view relativePath) const;

  [[nodiscard]] const PathGuard& pathGuard() const noexcept { return guard_; }

 private:
  PathGuard guard_;
};

}  // namespace wikicore::vault
