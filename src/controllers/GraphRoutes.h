#pragma once

#include "index/GraphQueries.h"

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

void registerGraphRoutes(drogon::HttpAppFramework& app, wikicore::index::GraphQueries& graph);

}  // namespace wikicore::controllers
