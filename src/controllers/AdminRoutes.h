#pragma once

#include "auth/McpRemoteConfig.h"
#include "embeddings/EmbeddingProvider.h"
#include "index/Database.h"
#include "index/IndexBuilder.h"
#include "index/McpAuditLog.h"

#include <drogon/HttpAppFramework.h>

#include <string>

namespace wikicore::controllers {

// POST /api/admin/reindex - admin+CSRF. Runs IndexBuilder::fullRescan()
// synchronously and returns {"documentsIndexed":N,"staleRowsRemoved":N}.
// Manual recovery path for "the index doesn't match the vault" (external
// edits made outside the app, or a deleted/corrupted index db) — the same
// rescan also runs unconditionally at every wiki-server startup.
//
// GET /api/admin/mcp-audit-log - admin only. Every recorded
// create_document/update_document call an MCP client made (success or
// failure, "remote:"-prefixed tool names for calls that came through the
// HTTP transport, RemoteMcpRoutes.cpp), newest first, capped at 200 rows
// — the accountability half of write access, local or remote alike.
//
// Remote MCP settings (McpRemoteConfig.h) — all admin+CSRF, all take
// effect immediately (no restart, the whole point of storing this in
// SQLite instead of config.toml):
//   GET    /api/admin/mcp-remote-config
//     -> {enabled, writeEnabled, hasToken, allowedCidrs: [...]}. Never
//        returns the token itself or its hash — hasToken is the only
//        signal a client gets that one exists.
//   PUT    /api/admin/mcp-remote-config   body: {enabled?, writeEnabled?}
//     -> flips only the field(s) present in the body; omitted fields are
//        left exactly as they were (see McpRemoteConfig::setEnabled/
//        setWriteEnabled's own "each setter only touches its own
//        column" guarantee).
//   POST   /api/admin/mcp-remote-config/regenerate-token
//     -> {token: "..."} — the RAW token, shown ONCE, right here. Nothing
//        else in this app ever displays it again; losing it means
//        regenerating a new one (which immediately invalidates the old).
//   POST   /api/admin/mcp-remote-config/allowed-cidrs   body: {cidr}
//     -> adds one allowlist entry (idempotent — adding the same one twice
//        is a no-op, not an error).
//   DELETE /api/admin/mcp-remote-config/allowed-cidrs?cidr=...
//     -> removes one (query param, not a body — a DELETE body is
//        stripped by some proxies/clients along the way). Removing the
//        last entry returns to "no IP
//        restriction configured" (allow), not "allow nothing" — see
//        McpRemoteConfig::isIpAllowed's own comment on why.
//
// GET /api/admin/backup - admin only, no CSRF (a pure GET that mutates
// nothing — same convention as /api/admin/mcp-audit-log above). Streams
// the entire vault directory as a .tar.gz (see vault/BackupService.h for
// exactly what that includes and how it's built) with a
// Content-Disposition that names the file `wiki-backup-<timestamp>.tar.gz`
// so a plain browser navigation to this URL downloads it directly — no
// client-side JS blob/fetch dance needed, same reasoning as why this route
// doesn't need CSRF: reading response bytes cross-origin is blocked by the
// browser regardless, and there's no state here to forge a change to.
//
// Embeddings admin (only registered on a WIKI_ENABLE_SQLITE_VEC build — see
// AdminRoutes.cpp; on any other build these routes simply don't exist, a
// plain 404, same as the frontend section that calls them staying hidden):
//   GET  /api/admin/embeddings-status
//     -> {configured, providerModel, dimensions, vectorSearchEnabled,
//         needingAttention: [{documentRowId, path, title, lastError}]}.
//        `configured` is false for provider="none" (providerModel=="none")
//        — see docs/embeddings.md's "Three real bugs" section on why this
//        check matters. `needingAttention` is
//        EmbeddingIndexer::listNeedingAttention() as-is: never attempted,
//        or last attempt failed — never documents merely due for a routine
//        hash-mismatch re-embed on next save.
//   PUT  /api/admin/embeddings-status   body: {vectorSearchEnabled}
//     -> flips the runtime toggle (EmbeddingsRuntimeConfig, no restart —
//        see docs/embeddings.md), returns the same body as GET.
//   POST /api/admin/embeddings-status/reembed   body: {path}
//     -> re-derives exactly that one document's index row (via
//        IndexBuilder::reindexOneFile, the same primitive VaultWatcher
//        uses), including a fresh embed attempt — NOT a full --reindex.
//        {"ok":true} or a 404 if the path no longer exists on disk.
//        Logged to mcp_audit_log as "admin:reembed" (see McpAuditLog.h),
//        success or failure — visible via GET /api/admin/mcp-audit-log
//        same as any MCP write tool call.
//   POST /api/admin/embeddings-status/reembed-all
//     -> calls the above for every document listNeedingAttention() returns
//        right now. {"attempted":N,"stillFailing":M} — stillFailing is a
//        fresh listNeedingAttention() count taken after all attempts, so a
//        provider that's still down shows up honestly rather than as a
//        false "fixed". Logged as ONE "admin:reembed-all" summary entry
//        (not one per document), success = stillFailing == 0.
void registerAdminRoutes(drogon::HttpAppFramework& app, wikicore::index::IndexBuilder& indexBuilder,
                          wikicore::index::McpAuditLog& mcpAuditLog,
                          wikicore::auth::McpRemoteConfig& mcpRemoteConfig,
                          const std::string& vaultPath, wikicore::index::Database& db,
                          wikicore::embeddings::EmbeddingProvider* embeddingProvider);

}  // namespace wikicore::controllers
