#pragma once

#include "index/FtsSearch.h"

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

// GET /search           - full page (search box + htmx-wired results)
// GET /api/search       - htmx partial: HTML fragment of matching
//                          documents for ?q=&tag=&type=, visibility-gated
//                          by the caller's auth state
void registerSearchRoutes(drogon::HttpAppFramework& app, wikicore::index::FtsSearch& search);

}  // namespace wikicore::controllers
