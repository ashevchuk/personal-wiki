#pragma once

#include "index/FtsSearch.h"
#include "index/IndexUpdater.h"
#include "index/NavQueries.h"
#include "vault/DocumentService.h"

#include <string>

namespace wikicore::mcp {

// Builds and runs the MCP stdio server exposing 4 read-only tools:
// search_documents, get_document, list_tags, list_documents. Blocks until
// stdin closes.
//
// `includePrivate` is fixed for the whole process lifetime, from
// config.toml's [mcp].scope ("admin" -> true, "public" -> false — see
// AppConfig). There is no per-request auth in stdio mode: the transport
// itself is the trust boundary (whoever can spawn this process already
// has filesystem access to the vault) — but every tool still re-checks a
// document's own visibility before returning it, the same fail-safe-
// private rule as everywhere else, rather than trusting includePrivate
// alone to have been threaded correctly through every code path.
void runServer(const std::string& serverName, const std::string& serverVersion,
               index::FtsSearch& search, index::NavQueries& nav,
               index::IndexUpdater& indexUpdater, vault::DocumentService& documents,
               bool includePrivate);

}  // namespace wikicore::mcp
