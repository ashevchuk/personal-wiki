#pragma once

#include "vault/FolderService.h"

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

// Registers:
//   POST   /api/folders/move          - {oldPath, newPath} in the JSON
//                                        body; moves an entire folder
//                                        subtree, reindexing every
//                                        document under it
//   DELETE /api/folders/{path...}     - removes an EMPTY folder only
//
// Both admin+CSRF, same as every other mutating route — see
// DocumentRoutes.h for the filter-name gotcha this mirrors.
void registerFolderRoutes(drogon::HttpAppFramework& app,
                           wikicore::vault::FolderService& folderService);

}  // namespace wikicore::controllers
