#pragma once

#include "vault/FolderService.h"

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

// Registers the folders JSON API:
//   POST   /api/folders/move          - {oldPath, newPath} in the JSON
//                                        body; moves an entire folder
//                                        subtree, reindexing every
//                                        document under it
//   DELETE /api/folders/{path...}     - removes an EMPTY folder only
//
// Both admin+CSRF, same as every other mutating route — see
// DocumentRoutes.h for the filter-name gotcha this mirrors.
//
// GET /folder and /folder/{path...} (the pages a human navigates to) are
// handled by PageRoutes.cpp instead — no HTML here, see
// docs/architecture.md's frontend section. The folder listing itself is
// fetched client-side from /api/nav/tree (see static/js/pages/folder.js),
// not a route in this file.
void registerFolderRoutes(drogon::HttpAppFramework& app,
                           wikicore::vault::FolderService& folderService);

}  // namespace wikicore::controllers
