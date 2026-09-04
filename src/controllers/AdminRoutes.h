#pragma once

#include "index/IndexBuilder.h"
#include "index/McpAuditLog.h"

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

// POST /api/admin/reindex - admin+CSRF. Runs IndexBuilder::fullRescan()
// synchronously and returns {"documentsIndexed":N,"staleRowsRemoved":N}.
// Manual recovery path for "the index doesn't match the vault" (external
// edits made outside the app, or a deleted/corrupted index db) — the same
// rescan also runs unconditionally at every wiki-server startup.
//
// GET /api/admin/mcp-audit-log - admin only. Every recorded
// create_document/update_document call an MCP client made (success or
// failure), newest first, capped at 200 rows — the accountability half
// of [mcp].write_access (McpServer.cpp/McpAuditLog.h).
void registerAdminRoutes(drogon::HttpAppFramework& app, wikicore::index::IndexBuilder& indexBuilder,
                          wikicore::index::McpAuditLog& mcpAuditLog);

}  // namespace wikicore::controllers
