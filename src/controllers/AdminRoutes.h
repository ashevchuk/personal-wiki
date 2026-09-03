#pragma once

#include "index/IndexBuilder.h"

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

// POST /api/admin/reindex - admin+CSRF. Runs IndexBuilder::fullRescan()
// synchronously and returns {"documentsIndexed":N,"staleRowsRemoved":N}.
// Manual recovery path for "the index doesn't match the vault" (external
// edits made outside the app, or a deleted/corrupted index db) — the same
// rescan also runs unconditionally at every wiki-server startup.
void registerAdminRoutes(drogon::HttpAppFramework& app, wikicore::index::IndexBuilder& indexBuilder);

}  // namespace wikicore::controllers
