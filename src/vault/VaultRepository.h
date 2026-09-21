#pragma once

#include "vault/PathGuard.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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
  void writeRawAtomic(std::string_view relativePath, std::string_view content) const;

  // Same atomic temp-file + rename as writeRawAtomic, but copies from an
  // already-on-disk source (streamed by the filesystem, never held as a
  // std::string). Used for large attachments. `source` is NOT a vault
  // path — PathGuard only constrains the destination.
  void copyFileAtomic(std::string_view relativePath,
                       const std::filesystem::path& source) const;

  // Moves the document at `relativePath`, and its co-located
  // "<stem>.assets/" folder if one exists, to the equivalent path under
  // ".trash/" (creating parent directories as needed). Throws
  // filesystem_error if the source doesn't exist.
  void moveToTrash(std::string_view relativePath) const;

  // Renames/moves the document at `oldRelativePath` to `newRelativePath`
  // (creating parent directories as needed) and, if present, its
  // co-located "<stem>.assets/" folder to the matching new stem. PathGuard
  // on both paths. Throws filesystem_error if the source doesn't exist or
  // the rename fails; if the document moved but its assets folder didn't,
  // surfaces that rather than rolling the document back (same discipline
  // as moveToTrash).
  void renameDocument(std::string_view oldRelativePath,
                      std::string_view newRelativePath) const;

  struct FileStat {
    int64_t size = 0;
    int64_t mtimeUnix = 0;
  };
  // Throws filesystem_error if the path doesn't exist.
  [[nodiscard]] FileStat statFile(std::string_view relativePath) const;

  // Regular files in a vault-relative directory (non-recursive). A
  // missing path, or a path that isn't a directory, returns empty —
  // listing something that isn't there is not an error. Skips dotfiles.
  // Each entry is vault-relative (`relativeDir` + filename).
  [[nodiscard]] std::vector<std::string> listRegularFiles(
      std::string_view relativeDir) const;

  // Removes a file (or an empty directory). Missing path is a no-op.
  // Throws PathTraversalError if it escapes the vault.
  void removeFile(std::string_view relativePath) const;

  [[nodiscard]] const PathGuard& pathGuard() const noexcept { return guard_; }

 private:
  PathGuard guard_;
};

}  // namespace wikicore::vault
