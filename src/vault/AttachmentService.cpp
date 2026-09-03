#include "vault/AttachmentService.h"

#include "util/Uuid.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_map>

namespace fs = std::filesystem;

namespace wikicore::vault {

namespace {

constexpr int64_t kMaxAttachmentBytes = 25LL * 1024 * 1024;  // 25 MiB

// Extension (lowercase, without dot) -> MIME type. Deliberately an
// allowlist, not a blocklist: anything not listed here is rejected.
const std::unordered_map<std::string, std::string>& allowedExtensions() {
  static const std::unordered_map<std::string, std::string> kMap = {
      {"png", "image/png"},        {"jpg", "image/jpeg"},
      {"jpeg", "image/jpeg"},      {"gif", "image/gif"},
      {"webp", "image/webp"},      {"svg", "image/svg+xml"},
      {"pdf", "application/pdf"},  {"txt", "text/plain"},
      {"md", "text/markdown"},     {"zip", "application/zip"},
      {"mp3", "audio/mpeg"},       {"mp4", "video/mp4"},
      {"webm", "video/webm"},      {"csv", "text/csv"},
      {"json", "application/json"},
  };
  return kMap;
}

std::string lowerAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                  [](unsigned char c) { return std::tolower(c); });
  return s;
}

// Keeps only [A-Za-z0-9._-], collapses anything else to '_'. Applied to
// just the basename (see store()) — this alone is enough to rule out path
// separators, NUL bytes, and the rest of PathGuard's usual concerns, but
// PathGuard still validates the final resolved path regardless.
std::string sanitizeBasename(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name) {
    const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
                     c == '-' || c == '_';
    out += ok ? c : '_';
  }
  return out;
}

// "notes/foo.md" -> "notes/foo.assets" — the co-located attachments
// folder for a document, per the plan's storage layout.
std::string assetsDirFor(const std::string& documentRelativePath) {
  const fs::path doc(documentRelativePath);
  const fs::path assetsDir = doc.parent_path() / (doc.stem().string() + ".assets");
  return assetsDir.generic_string();
}

}  // namespace

AttachmentInfo AttachmentService::store(const std::string& documentRelativePath,
                                         const std::string& originalFilename,
                                         const std::string& content) {
  if (content.size() > static_cast<size_t>(kMaxAttachmentBytes)) {
    throw AttachmentRejectedError("attachment exceeds the 25 MiB size limit");
  }

  const std::string extension =
      lowerAscii(fs::path(originalFilename).extension().string());
  const std::string extNoDot = extension.empty() ? "" : extension.substr(1);
  const auto& allowed = allowedExtensions();
  const auto it = allowed.find(extNoDot);
  if (it == allowed.end()) {
    throw AttachmentRejectedError("file type not allowed: ." + extNoDot);
  }

  std::string sanitized = sanitizeBasename(fs::path(originalFilename).filename().string());
  // A name that sanitizes down to nothing usable (empty, or exactly "."
  // /".." which sanitizeBasename can't produce directly but a
  // pathological input like "..." could still collapse toward) gets a
  // fresh generated name instead of being trusted further.
  if (sanitized.empty() || sanitized == "." || sanitized == "..") {
    sanitized = util::newUuidV4() + "." + extNoDot;
  }

  const std::string assetsDir = assetsDirFor(documentRelativePath);
  std::string relativePath = assetsDir + "/" + sanitized;

  // De-dupe: if that name is already taken in this document's assets
  // folder, prefix a short random suffix rather than silently overwriting
  // someone's earlier upload.
  if (vault_.exists(relativePath)) {
    const fs::path p(sanitized);
    sanitized = p.stem().string() + "-" + util::newUuidV4().substr(0, 8) +
                p.extension().string();
    relativePath = assetsDir + "/" + sanitized;
  }

  vault_.writeRawAtomic(relativePath, content);

  AttachmentInfo info;
  info.relativePath = relativePath;
  info.mimeType = it->second;
  info.size = static_cast<int64_t>(content.size());
  return info;
}

}  // namespace wikicore::vault
