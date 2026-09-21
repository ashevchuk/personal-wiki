#pragma once

#include "index/FtsSearch.h"
#include "index/GraphQueries.h"
#include "vault/VaultRepository.h"

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

void registerGraphRoutes(drogon::HttpAppFramework& app,
                         wikicore::index::GraphQueries& graph,
                         wikicore::index::FtsSearch& search,
                         wikicore::vault::VaultRepository& vault);

}  // namespace wikicore::controllers
