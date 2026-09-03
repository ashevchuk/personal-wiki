#pragma once

#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace wikicore::vault {

// Thrown by PathGuard::resolve() when a requested path would escape the
// vault root, or is otherwise malformed (absolute, empty, embedded NUL,
// ".." segments, a symlink pointing outside the vault, ...).
class PathTraversalError : public std::runtime_error {
 public:
  explicit PathTraversalError(std::string_view requestedPath);
};

// The single point of contact between "a path string from the outside
// world" (an HTTP route segment, a front-matter attachment reference, a
// CLI arg) and the real filesystem. Every access to vault content MUST go
// through this class; nothing else in the codebase is allowed to build a
// vault-relative std::filesystem::path of its own. See docs/architecture.md,
// "Filesystem access is centralized".
class PathGuard {
 public:
  // `vaultRoot` must already exist; it is canonicalized once here and
  // cached. Throws std::filesystem::filesystem_error if it doesn't exist
  // or isn't a directory.
  explicit PathGuard(std::filesystem::path vaultRoot);

  // Resolves `requestedRelativePath` (an untrusted, '/'-separated relative
  // path, already URL-decoded by the caller) against the vault root.
  //
  // Throws PathTraversalError if the result would fall outside the vault
  // root — via ".." segments, an absolute path, an embedded NUL byte, or a
  // symlink that resolves outside. Existing path components are resolved
  // through symlinks before the containment check; a non-existent tail
  // (e.g. a file about to be created) can't be symlink-resolved yet, so it
  // is validated lexically instead — this is why the check runs twice, see
  // the .cpp.
  //
  // Does NOT check that the result exists — callers that need an existing
  // file get std::filesystem::filesystem_error from whatever they do with
  // the returned path.
  [[nodiscard]] std::filesystem::path resolve(
      std::string_view requestedRelativePath) const;

  [[nodiscard]] const std::filesystem::path& root() const noexcept {
    return vaultRoot_;
  }

 private:
  std::filesystem::path vaultRoot_;  // canonical, set once at construction
};

}  // namespace wikicore::vault
