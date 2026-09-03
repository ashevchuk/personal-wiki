#pragma once

#include "vault/FolderService.h"

#include <drogon/HttpAppFramework.h>

#include <string>

namespace wikicore::controllers {

// Registers:
//   GET    /folder                    - browse the vault root
//   GET    /folder/{path...}          - browse everything directly under
//                                        `path` (subfolders + documents);
//                                        content is fetched client-side
//                                        from the existing /api/nav/tree
//                                        (see static/js/folder.js) — no
//                                        new listing endpoint needed
//   POST   /api/folders/move          - {oldPath, newPath} in the JSON
//                                        body; moves an entire folder
//                                        subtree, reindexing every
//                                        document under it
//   DELETE /api/folders/{path...}     - removes an EMPTY folder only
//
// The two mutating routes are admin+CSRF, same as every other mutating
// route — see DocumentRoutes.h for the filter-name gotcha this mirrors.
// `basePath` (already normalized, see AppConfig::basePath) — see
// AuthRoutes.h's doc comment for what it does and why.
void registerFolderRoutes(drogon::HttpAppFramework& app,
                           wikicore::vault::FolderService& folderService,
                           const std::string& basePath);

}  // namespace wikicore::controllers
