#pragma once

#include "vault/AttachmentService.h"
#include "vault/DocumentService.h"

#include <filesystem>
#include <string>

namespace wikicore::vault {

struct AttachedFile {
  AttachmentInfo info;
  // The single markdown line appended to the owning document (image
  // embed for inline-safe raster types, a regular link otherwise). No
  // surrounding blank lines — attachFileAndLink adds those when it
  // writes the body.
  std::string markdownLink;
};

// Stores a file next to `documentPath` via AttachmentService (same
// sanitization / de-dupe as POST /api/attachments), then appends a
// markdown link to the document body via DocumentService::update so the
// edit is snapshotted like any other save. There is no size cap on the
// file itself — `content` is the in-memory form (MCP content_base64);
// attachFileAndLinkFromPath streams from disk instead.
//
// The href is `assets/` + the vault-relative attachment path, no leading
// slash — the same <base href="{basePath}/"> resolution WikiLinks uses
// for `d/{path}`, so it works behind a reverse-proxy subpath. Throws
// DocumentNotFoundError if the owning document doesn't exist yet
// (attachments have nowhere to live otherwise, same as the HTTP upload
// route), PathTraversalError / AttachmentRejectedError from the
// underlying services.
AttachedFile attachFileAndLink(DocumentService& documents, AttachmentService& attachments,
                                const std::string& documentPath,
                                const std::string& originalFilename,
                                const std::string& content);

// Same as attachFileAndLink, but copies `sourcePath` from the host
// filesystem (streamed, never loaded as a std::string). This is how a
// 120 MiB PDF gets into the vault from an MCP client — the model passes
// a path, not the bytes.
AttachedFile attachFileAndLinkFromPath(DocumentService& documents,
                                        AttachmentService& attachments,
                                        const std::string& documentPath,
                                        const std::string& originalFilename,
                                        const std::filesystem::path& sourcePath);

}  // namespace wikicore::vault
