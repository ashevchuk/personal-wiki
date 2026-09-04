#pragma once

#include "index/FtsSearch.h"
#include "index/IndexUpdater.h"
#include "index/McpAuditLog.h"
#include "index/NavQueries.h"
#include "vault/DocumentService.h"

#include <string>

namespace wikicore::mcp {

// Builds and runs the MCP stdio server exposing the 4 read-only tools
// (search_documents, get_document, list_tags, list_documents), plus,
// when `writeAccess` is true, create_document/update_document. Blocks
// until stdin closes.
//
// `includePrivate` is fixed for the whole process lifetime, from
// config.toml's [mcp].scope ("admin" -> true, "public" -> false — see
// AppConfig). There is no per-request auth in stdio mode: the transport
// itself is the trust boundary (whoever can spawn this process already
// has filesystem access to the vault) — but every tool still re-checks a
// document's own visibility before returning it, the same fail-safe-
// private rule as everywhere else, rather than trusting includePrivate
// alone to have been threaded correctly through every code path.
//
// `writeAccess` is [mcp].write_access, default false — see AppConfig's
// own comment on why this is an explicit opt-in. When false, the write
// tools aren't registered at all (absent from tools/list, not merely
// present-and-erroring) — exactly what "behind a flag" means here.
// `auditLog` records every write attempt, success or failure, regardless
// of this flag's current value (a write made while the flag was on stays
// in the log even after it's turned back off).
void runServer(const std::string& serverName, const std::string& serverVersion,
               index::FtsSearch& search, index::NavQueries& nav,
               index::IndexUpdater& indexUpdater, vault::DocumentService& documents,
               index::McpAuditLog& auditLog, bool includePrivate, bool writeAccess);

}  // namespace wikicore::mcp
