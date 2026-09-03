#pragma once

#include "index/NavQueries.h"

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

// GET /api/nav/tree - JSON array of {path, title, visibility} for every
//                     document visible to the caller, ordered by path.
//                     Flat, not nested — building the folder tree from
//                     path segments is left to whatever consumes this
//                     (no dedicated nav sidebar view exists yet).
// GET /api/nav/tags - JSON array of {tag, count} of visible documents.
void registerNavRoutes(drogon::HttpAppFramework& app, wikicore::index::NavQueries& nav);

}  // namespace wikicore::controllers
