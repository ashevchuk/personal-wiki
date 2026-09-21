#include "vault/AttachToDocument.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace wikicore::vault {

namespace {

std::string lowerAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                  [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string extensionNoDot(const std::string& relativePath) {
  std::string ext = fs::path(relativePath).extension().string();
  if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
  return lowerAscii(std::move(ext));
}

// Raster images the browser can render as <img> AND that GET /assets/
// will serve inline (svg is image/svg+xml but forced-download — an
// <img> of it wouldn't display, so it becomes a regular link instead).
bool isInlineImage(const AttachmentService& attachments, const AttachmentInfo& info) {
  if (info.mimeType.rfind("image/", 0) != 0) return false;
  return attachments.isSafeToRenderInline(extensionNoDot(info.relativePath));
}

std::string markdownLinkFor(const AttachmentService& attachments, const AttachmentInfo& info) {
  const std::string storedName = fs::path(info.relativePath).filename().generic_string();
  // Relative to <base href="{basePath}/">, matching WikiLinks' `d/` hrefs
  // — see that header's comment on why a leading '/' would break behind
  // a reverse-proxy subpath.
  const std::string href = "assets/" + info.relativePath;
  if (isInlineImage(attachments, info)) {
    return "![" + storedName + "](" + href + ")";
  }
  return "[" + storedName + "](" + href + ")";
}

std::string appendLink(std::string body, const std::string& link) {
  if (body.empty()) return link + "\n";
  if (body.back() != '\n') body += '\n';
  body += '\n';
  body += link;
  body += '\n';
  return body;
}

AttachedFile finishAttach(DocumentService& documents, AttachmentService& attachments,
                           const DocumentRecord& existing, const std::string& documentPath,
                           const AttachmentInfo& info) {
  const std::string markdownLink = markdownLinkFor(attachments, info);

  DocumentInput input;
  input.title = existing.frontMatter.title;
  input.tags = existing.frontMatter.tags;
  input.visibility = existing.frontMatter.visibility;
  input.type = existing.frontMatter.type;
  input.body = appendLink(existing.body, markdownLink);
  documents.update(documentPath, input);

  return AttachedFile{info, markdownLink};
}

}  // namespace

AttachedFile attachFileAndLink(DocumentService& documents, AttachmentService& attachments,
                                const std::string& documentPath,
                                const std::string& originalFilename,
                                const std::string& content) {
  // Existence check FIRST — AttachmentService::store derives the assets
  // folder from the path string and will happily write it even if no
  // document lives there. The HTTP upload route refuses that with 404;
  // this matches. get() (not vault.exists) so a traversal attempt is
  // PathTraversalError rather than a generic not-found, the same
  // distinction DocumentService itself makes.
  const DocumentRecord existing = documents.get(documentPath);
  const AttachmentInfo info = attachments.store(documentPath, originalFilename, content);
  return finishAttach(documents, attachments, existing, documentPath, info);
}

AttachedFile attachFileAndLinkFromPath(DocumentService& documents,
                                        AttachmentService& attachments,
                                        const std::string& documentPath,
                                        const std::string& originalFilename,
                                        const fs::path& sourcePath) {
  const DocumentRecord existing = documents.get(documentPath);
  const AttachmentInfo info =
      attachments.storeFromPath(documentPath, originalFilename, sourcePath);
  return finishAttach(documents, attachments, existing, documentPath, info);
}

}  // namespace wikicore::vault
