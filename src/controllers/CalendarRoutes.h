#pragma once

#include "index/CalendarQueries.h"

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

void registerCalendarRoutes(drogon::HttpAppFramework& app,
                            wikicore::index::CalendarQueries& calendar);

}  // namespace wikicore::controllers
