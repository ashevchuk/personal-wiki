#pragma once

#include "index/IndexUpdater.h"
#include "index/SnapshotStore.h"
#include "vault/DocumentService.h"

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

// Phase 2 versioning — read/restore access to what SnapshotStore records
// on every DocumentService::update (see its own doc comment). Admin-only,
// same as every other mutating/editorial concern in this app: there's no
// public "view history" feature, matching the single-admin/no-multi-user
// model everywhere else (docs/architecture.md).
//
// Deliberately NOT nested under /api/documents/{path...} — that prefix
// already has a greedy "^/api/documents/(.*)$" regex registered for
// GET/PUT/DELETE (see DocumentRoutes.cpp); adding another regex under
// the same prefix would mean relying on Drogon's own tie-breaking rule
// between two matching patterns instead of making the ambiguity
// impossible by construction. A distinct URL prefix sidesteps that
// entirely.
//
//   GET  /api/document-history/{path...}         - list of {id,
//                                                   snapshotAt} for that
//                                                   document, newest
//                                                   first. 404 if the
//                                                   path isn't indexed.
//   GET  /api/document-history/{path...}?id=N     - one snapshot's
//                                                   content, parsed the
//                                                   same shape as GET
//                                                   /api/documents/{path}
//                                                   (title/tags/type/
//                                                   visibility/body) so
//                                                   the frontend can diff
//                                                   `body` against the
//                                                   live document the
//                                                   same way. 404 if `id`
//                                                   doesn't belong to
//                                                   THIS document (see
//                                                   SnapshotStore::getContent).
//   POST /api/document-restore/{path...}?id=N     - re-saves snapshot
//                                                   `id`'s content as the
//                                                   document's current
//                                                   state, through the
//                                                   normal
//                                                   DocumentService::update
//                                                   path — which itself
//                                                   snapshots the
//                                                   pre-restore state
//                                                   first, so a restore
//                                                   is undoable the same
//                                                   way any other edit is.
void registerVersionRoutes(drogon::HttpAppFramework& app,
                            wikicore::index::IndexUpdater& indexUpdater,
                            wikicore::index::SnapshotStore& snapshots,
                            wikicore::vault::DocumentService& documentService);

}  // namespace wikicore::controllers
