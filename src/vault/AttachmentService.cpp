#include "vault/AttachmentService.h"

#include "util/Uuid.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace wikicore::vault {

namespace {

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

struct PlannedDest {
  std::string relativePath;
  std::string extNoDot;
};

PlannedDest planDest(VaultRepository& vault, const std::string& documentRelativePath,
                      const std::string& originalFilename) {
  const std::string extension =
      lowerAscii(fs::path(originalFilename).extension().string());
  const std::string extNoDot = extension.empty() ? "" : extension.substr(1);

  std::string sanitized = sanitizeBasename(fs::path(originalFilename).filename().string());
  // A name that sanitizes down to nothing usable (empty, or exactly "."
  // /".." which sanitizeBasename can't produce directly but a
  // pathological input like "..." could still collapse toward) gets a
  // fresh generated name instead of being trusted further.
  if (sanitized.empty() || sanitized == "." || sanitized == "..") {
    sanitized = util::newUuidV4() + (extNoDot.empty() ? "" : "." + extNoDot);
  }

  const std::string assetsDir = assetsDirFor(documentRelativePath);
  std::string relativePath = assetsDir + "/" + sanitized;

  // De-dupe: if that name is already taken in this document's assets
  // folder, prefix a short random suffix rather than silently overwriting
  // someone's earlier upload.
  if (vault.exists(relativePath)) {
    const fs::path p(sanitized);
    sanitized = p.stem().string() + "-" + util::newUuidV4().substr(0, 8) +
                p.extension().string();
    relativePath = assetsDir + "/" + sanitized;
  }
  return {relativePath, extNoDot};
}

}  // namespace

const std::unordered_map<std::string, std::string>& AttachmentService::defaultMimeTypes() {
  static const std::unordered_map<std::string, std::string> kMap = {
      {"png", "image/png"},         {"jpg", "image/jpeg"},
      {"jpeg", "image/jpeg"},       {"gif", "image/gif"},
      {"webp", "image/webp"},       {"svg", "image/svg+xml"},
      {"pdf", "application/pdf"},   {"txt", "text/plain"},
      {"md", "text/markdown"},      {"zip", "application/zip"},
      {"mp3", "audio/mpeg"},        {"mp4", "video/mp4"},
      {"webm", "video/webm"},       {"csv", "text/csv"},
      {"json", "application/json"}, {"yaml", "text/yaml"},
      {"yml", "text/yaml"},         {"toml", "text/plain"},
      {"ini", "text/plain"},        {"conf", "text/plain"},
      {"cfg", "text/plain"},        {"log", "text/plain"},
      {"xml", "application/xml"},   {"html", "text/html"},
      {"htm", "text/html"},         {"css", "text/css"},
      {"js", "text/javascript"},    {"sh", "text/x-shellscript"},
      {"py", "text/x-python"},      {"gz", "application/gzip"},
      {"tar", "application/x-tar"}, {"7z", "application/x-7z-compressed"},
      {"doc", "application/msword"},
      {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
      {"xls", "application/vnd.ms-excel"},
      {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
      {"ogg", "audio/ogg"},         {"wav", "audio/wav"},
      {"mov", "video/quicktime"},   {"avi", "video/x-msvideo"},
  };
  return kMap;
}

// Deliberately small and conservative: only formats with no plausible way
// to execute script same-origin when a browser navigates straight to
// GET /assets/{path...}. Notably NOT here even though defaultMimeTypes()
// above knows their type: html/htm (obviously), svg (can embed <script>),
// xml (XSLT can execute), js/css (not dangerous to RENDER, but browsers
// doing MIME-sniffing on an inline text/plain-ish response is not a
// fight worth having — force download for these too).
const std::unordered_set<std::string>& AttachmentService::defaultInlineSafeExtensions() {
  static const std::unordered_set<std::string> kSet = {
      "png", "jpg", "jpeg", "gif", "webp", "pdf", "txt", "md",
      "mp3", "mp4", "webm", "csv", "json", "ogg", "wav",
  };
  return kSet;
}

std::string AttachmentService::mimeTypeForExtension(const std::string& extensionNoDot) const {
  const auto it = mimeTypes_.find(lowerAscii(extensionNoDot));
  return it != mimeTypes_.end() ? it->second : "application/octet-stream";
}

bool AttachmentService::isSafeToRenderInline(const std::string& extensionNoDot) const {
  return inlineSafeExtensions_.count(lowerAscii(extensionNoDot)) > 0;
}

AttachmentInfo AttachmentService::store(const std::string& documentRelativePath,
                                         const std::string& originalFilename,
                                         const std::string& content) {
  const PlannedDest dest = planDest(vault_, documentRelativePath, originalFilename);
  vault_.writeRawAtomic(dest.relativePath, content);

  AttachmentInfo info;
  info.relativePath = dest.relativePath;
  info.mimeType = mimeTypeForExtension(dest.extNoDot);
  info.size = static_cast<int64_t>(content.size());
  return info;
}

AttachmentInfo AttachmentService::storeFromPath(const std::string& documentRelativePath,
                                                 const std::string& originalFilename,
                                                 const fs::path& sourcePath) {
  std::error_code statEc;
  const auto st = fs::status(sourcePath, statEc);
  if (statEc || !fs::exists(st)) {
    throw AttachmentRejectedError("source file not found");
  }
  if (!fs::is_regular_file(st)) {
    throw AttachmentRejectedError("source is not a regular file");
  }

  const PlannedDest dest = planDest(vault_, documentRelativePath, originalFilename);
  vault_.copyFileAtomic(dest.relativePath, sourcePath);

  AttachmentInfo info;
  info.relativePath = dest.relativePath;
  info.mimeType = mimeTypeForExtension(dest.extNoDot);
  info.size = static_cast<int64_t>(fs::file_size(sourcePath));
  return info;
}

std::vector<AttachmentInfo> AttachmentService::listForDocument(
    const std::string& documentRelativePath) const {
  const std::string assetsDir = assetsDirFor(documentRelativePath);
  std::vector<std::string> paths = vault_.listRegularFiles(assetsDir);
  std::sort(paths.begin(), paths.end());

  std::vector<AttachmentInfo> out;
  out.reserve(paths.size());
  for (const auto& rel : paths) {
    AttachmentInfo info;
    info.relativePath = rel;
    std::string ext = fs::path(rel).extension().string();
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    info.mimeType = mimeTypeForExtension(ext);
    try {
      info.size = vault_.statFile(rel).size;
    } catch (const std::exception&) {
      continue;
    }
    out.push_back(std::move(info));
  }
  return out;
}

bool AttachmentService::remove(const std::string& assetRelativePath) {
  const fs::path p(assetRelativePath);
  const std::string dirName = p.parent_path().filename().string();
  constexpr std::string_view kSuffix = ".assets";
  if (dirName.size() <= kSuffix.size() ||
      dirName.compare(dirName.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
    throw AttachmentRejectedError("not an attachment path");
  }
  const std::string filename = p.filename().string();
  if (filename.empty() || filename == "." || filename == ".." || filename.front() == '.') {
    throw AttachmentRejectedError("not an attachment path");
  }

  if (!vault_.exists(assetRelativePath)) return false;
  vault_.removeFile(assetRelativePath);

  const std::string assetsDir = p.parent_path().generic_string();
  if (vault_.listRegularFiles(assetsDir).empty()) {
    vault_.removeFile(assetsDir);
  }
  return true;
}

}  // namespace wikicore::vault
