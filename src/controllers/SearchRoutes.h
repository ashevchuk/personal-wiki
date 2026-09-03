#pragma once

#include "index/FtsSearch.h"

#include <drogon/HttpAppFramework.h>

#include <string>

namespace wikicore::controllers {

// GET /search           - full page (search box + htmx-wired results)
// GET /api/search       - htmx partial: HTML fragment of matching
//                          documents for ?q=&tag=&type=, visibility-gated
//                          by the caller's auth state
//
// `basePath` (already normalized, see AppConfig::basePath) — see
// AuthRoutes.h's doc comment for what it does and why.
void registerSearchRoutes(drogon::HttpAppFramework& app, wikicore::index::FtsSearch& search,
                           const std::string& basePath);

}  // namespace wikicore::controllers
