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

// Throws PathTraversalError when any '/'-separated segment of a
// caller-supplied path starts with '.'. PathGuard::resolve deliberately
// allows such paths (the app itself keeps .trash/, .mcp-uploads/ and the
// index db under the vault root); this is the extra gate for paths that
// arrive from outside — HTTP routes, MCP tools, agent tools — so those
// internal areas are never addressable as documents, folders or assets.
void requireVisiblePath(std::string_view requestedRelativePath);

// True when the path lies inside a co-located `<doc-name>.assets/` folder.
// Files there are attachments, gated by their owning document's
// visibility; they are never documents themselves, whatever their
// extension or content.
[[nodiscard]] bool isInsideAssetsFolder(std::string_view relativePath);

// requireVisiblePath plus: the path must not lie inside an attachments
// folder. For every caller-supplied path that names a document.
void requireDocumentPath(std::string_view requestedRelativePath);

}  // namespace wikicore::vault
