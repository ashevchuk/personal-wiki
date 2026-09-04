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
// GET /api/nav/types - JSON array of {type, count} of doc_type values in
//                      use among visible documents. Backs the search
//                      page's type multiselect the same way /api/nav/tags
//                      backs its tag multiselect.
void registerNavRoutes(drogon::HttpAppFramework& app, wikicore::index::NavQueries& nav);

}  // namespace wikicore::controllers
