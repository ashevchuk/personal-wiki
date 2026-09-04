#pragma once

#include "index/NavQueries.h"
#include "vault/AttachmentService.h"
#include "vault/DocumentService.h"
#include "vault/VaultRepository.h"

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

// Registers the documents JSON API — no HTML anywhere here, see
// docs/architecture.md's frontend section. The /d/{path...} and
// /edit/{path...} URLs a human navigates to are handled by
// PageRoutes.cpp instead (they just serve the static SPA shell); this
// file is purely the data layer those pages' JS fetches from.
//
//   GET    /api/documents/{path...}      - read (visibility-gated same
//                                           as everything else: 404, not
//                                           403, for a private document
//                                           to an anonymous caller).
//                                           renderedHtml has [[wiki-links]]
//                                           already rewritten to real
//                                           <a> tags; "backlinks" lists
//                                           every OTHER document that
//                                           links here (NavQueries).
//   GET    /api/documents/{path...}/raw  - literal file bytes (unchanged)
//   POST   /api/documents                - create (path in the JSON body)
//   PUT    /api/documents/{path...}      - update
//   DELETE /api/documents/{path...}      - soft-delete (-> .trash/)
//   POST   /api/attachments/{path...}    - upload, path = owning document
//   GET    /assets/{path...}             - serve an attachment, visibility-
//                                           gated through its owning
//                                           document
//
// All mutating routes require AuthFilter + CsrfFilter (admin session +
// matching CSRF token) — see main.cpp for why the filters need a forced
// classTypeName() call, and docs/architecture.md for the fully-qualified-
// name gotcha in the {Get, "wikicore::auth::AuthFilter"}-style strings
// below.
void registerDocumentRoutes(drogon::HttpAppFramework& app,
                             wikicore::vault::VaultRepository& vault,
                             wikicore::vault::DocumentService& documentService,
                             wikicore::vault::AttachmentService& attachmentService,
                             wikicore::index::NavQueries& nav);

}  // namespace wikicore::controllers
