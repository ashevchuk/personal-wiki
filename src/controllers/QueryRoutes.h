#pragma once

#include "index/QueryBlocks.h"

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

void registerQueryRoutes(drogon::HttpAppFramework& app, wikicore::index::QueryBlocks& queryBlocks);

}  // namespace wikicore::controllers
