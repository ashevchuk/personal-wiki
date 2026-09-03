#pragma once

#include "vault/AttachmentService.h"
#include "vault/DocumentService.h"
#include "vault/VaultRepository.h"

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

// Registers:
//   GET    /d/{path...}              - render (M1)
//   GET    /edit/{path...}           - WYSIWYG edit page, admin-only,
//                                       works for an existing OR not-yet-
//                                       existing path (the latter = "new
//                                       document at this path")
//   POST   /api/documents            - create (path in the JSON body)
//   PUT    /api/documents/{path...}  - update
//   DELETE /api/documents/{path...}  - soft-delete (-> .trash/)
//   POST   /api/attachments/{path...} - upload, path = owning document
//   GET    /assets/{path...}         - serve an attachment, visibility-
//                                       gated through its owning document
//
// All mutating routes require AuthFilter + CsrfFilter (admin session +
// matching CSRF token) — see main.cpp for why the filters need a forced
// classTypeName() call, and docs/architecture.md for the fully-qualified-
// name gotcha in the {Get, "wikicore::auth::AuthFilter"}-style strings
// below.
void registerDocumentRoutes(drogon::HttpAppFramework& app,
                             wikicore::vault::VaultRepository& vault,
                             wikicore::vault::DocumentService& documentService,
                             wikicore::vault::AttachmentService& attachmentService);

}  // namespace wikicore::controllers
