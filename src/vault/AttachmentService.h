#pragma once

#include "vault/VaultRepository.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

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
//
// Any file extension is accepted on upload — there is no type allowlist
// or blocklist here (there used to be an allowlist; removed deliberately,
// see git history). The actual safety concern an extension policy was
// standing in for — a browser executing an uploaded file's content
// in-origin when navigated to directly (worst case: an uploaded .html or
// .svg with an embedded <script>) — is handled on the SERVING side
// instead (isSafeToRenderInline(), used by GET /assets/{path...} to force
// Content-Disposition: attachment for anything not on the curated "known
// safe to render inline" list), which restricts nothing about what can
// be uploaded or downloaded, only whether it's allowed to execute
// same-origin when opened directly in a browser tab.
//
// Both the MIME-type table and the inline-safe list are DATA, not
// business logic — configurable from config.toml's [attachments] section
// (see config.example.toml and AppConfig::attachmentMimeTypes /
// attachmentInlineSafeExtensions) rather than hardcoded here. The
// default*() static methods below back AppConfig's own defaults (used
// when config.toml doesn't override them) and double as this class's own
// defaults for callers (tests, mainly) that don't need to customize
// anything.
class AttachmentService {
 public:
  static const std::unordered_map<std::string, std::string>& defaultMimeTypes();
  static const std::unordered_set<std::string>& defaultInlineSafeExtensions();

  explicit AttachmentService(
      VaultRepository& vault,
      std::unordered_map<std::string, std::string> mimeTypes = defaultMimeTypes(),
      std::unordered_set<std::string> inlineSafeExtensions = defaultInlineSafeExtensions())
      : vault_(vault),
        mimeTypes_(std::move(mimeTypes)),
        inlineSafeExtensions_(std::move(inlineSafeExtensions)) {}

  // Throws AttachmentRejectedError only if `content` exceeds the size
  // cap. Throws PathTraversalError if `documentRelativePath` itself
  // escapes the vault.
  AttachmentInfo store(const std::string& documentRelativePath,
                        const std::string& originalFilename,
                        const std::string& content);

  // Best-effort MIME type from a file's extension (lowercase, without the
  // dot) — falls back to "application/octet-stream" for anything not
  // recognized. Used both by store() (for the response) and by
  // DocumentRoutes.cpp's /assets/ handler (to set Content-Type
  // consistently for files that predate this being computed at upload
  // time, or were dropped into the vault directly rather than through
  // this service).
  std::string mimeTypeForExtension(const std::string& extensionNoDot) const;

  // True for the configured set of types (images, PDF, audio, video,
  // plain text/markdown/CSV/JSON by default) that are safe to let a
  // browser render inline when navigating directly to
  // GET /assets/{path...}. Everything else gets served with
  // Content-Disposition: attachment instead — this is what neutralizes
  // the "uploaded .html/.svg executes same-origin JS" risk without
  // restricting what can be uploaded or downloaded at all.
  bool isSafeToRenderInline(const std::string& extensionNoDot) const;

 private:
  VaultRepository& vault_;
  std::unordered_map<std::string, std::string> mimeTypes_;
  std::unordered_set<std::string> inlineSafeExtensions_;
};

}  // namespace wikicore::vault
