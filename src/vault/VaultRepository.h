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

  // False for a path that escapes the vault too — existence of something
  // outside the vault is not this method's business to report.
  [[nodiscard]] bool exists(std::string_view relativePath) const;

  // Atomic: writes to a sibling temp file, then renames it over the
  // target (POSIX rename() is atomic within the same filesystem, which a
  // temp file in the same directory always is). Creates parent
  // directories as needed. Throws PathTraversalError / filesystem_error.
  void writeRawAtomic(std::string_view relativePath, const std::string& content) const;

  // Moves the document at `relativePath`, and its co-located
  // "<stem>.assets/" folder if one exists, to the equivalent path under
  // ".trash/" (creating parent directories as needed). Throws
  // filesystem_error if the source doesn't exist.
  void moveToTrash(std::string_view relativePath) const;

  struct FileStat {
    int64_t size = 0;
    int64_t mtimeUnix = 0;
  };
  // Throws filesystem_error if the path doesn't exist.
  [[nodiscard]] FileStat statFile(std::string_view relativePath) const;

  [[nodiscard]] const PathGuard& pathGuard() const noexcept { return guard_; }

 private:
  PathGuard guard_;
};

}  // namespace wikicore::vault
