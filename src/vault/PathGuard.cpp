#include "vault/PathGuard.h"

#include <string>

namespace fs = std::filesystem;

namespace wikicore::vault {

namespace {

// True iff `candidate` is `base` itself or lexically nested under it.
// Uses lexically_relative (component-wise) rather than string-prefix
// matching, so e.g. base "/data/vault" doesn't spuriously accept
// "/data/vault-evil".
bool isWithin(const fs::path& base, const fs::path& candidate) {
  const fs::path rel = candidate.lexically_relative(base);
  if (rel.empty()) return false;
  return *rel.begin() != "..";
}

}  // namespace

PathTraversalError::PathTraversalError(std::string_view requestedPath)
    : std::runtime_error("path escapes vault root: " +
                          std::string(requestedPath)) {}

PathGuard::PathGuard(fs::path vaultRoot)
    : vaultRoot_(fs::canonical(std::move(vaultRoot))) {}

fs::path PathGuard::resolve(std::string_view requestedRelativePath) const {
  if (requestedRelativePath.empty() ||
      requestedRelativePath.find('\0') != std::string_view::npos) {
    throw PathTraversalError(requestedRelativePath);
  }

  const fs::path requested(requestedRelativePath);
  if (requested.is_absolute()) {
    throw PathTraversalError(requestedRelativePath);
  }

  // Pass 1 — lexical: catches ".." escapes without touching the
  // filesystem. This also works when the target doesn't exist yet (e.g. a
  // new document about to be created), which pass 2 alone cannot do.
  const fs::path combined = (vaultRoot_ / requested).lexically_normal();
  if (!isWithin(vaultRoot_, combined)) {
    throw PathTraversalError(requestedRelativePath);
  }

  // Pass 2 — filesystem: weakly_canonical resolves symlinks in the
  // longest *existing* prefix of the path (any non-existent tail is left
  // untouched, syntactically). This is what catches a symlink planted
  // inside the vault that points outside it — something pass 1 can't see,
  // since it never touches the filesystem.
  const fs::path resolved = fs::weakly_canonical(combined);
  if (!isWithin(vaultRoot_, resolved)) {
    throw PathTraversalError(requestedRelativePath);
  }

  return resolved;
}

}  // namespace wikicore::vault
