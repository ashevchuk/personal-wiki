// wiki-mcp — MCP stdio entrypoint.
//
// Deliberately NOT linked against Drogon: Claude Desktop/Code spawn this
// process directly per-session, so it must start instantly and carry no
// HTTP-stack weight. MCP tools are read-only in the MVP (no write access) —
// see docs/mcp.md for the client-config example and docs/architecture.md
// for the design rationale.
//
// Unlike wiki-server, this does NOT rescan the vault at startup — a full
// walk on every spawn would defeat "starts instantly" for an MCP client
// that may launch this process often. It trusts whatever index already
// exists (kept current by wiki-server's own startup rescan / DocumentService
// writes / `wiki-server --reindex`). If the index looks stale, run
// `wiki-server --reindex` — not this binary.

#include "config/AppConfig.h"
#include "core/wikicore.h"
#include "index/Database.h"
#include "index/FtsSearch.h"
#include "index/IndexUpdater.h"
#include "index/NavQueries.h"
#include "mcp/McpServer.h"
#include "vault/DocumentService.h"
#include "vault/VaultRepository.h"

#include <cstdio>
#include <filesystem>

int main() {
  // stderr only, always -- see McpServer.cpp's comment on why stdout is
  // reserved exclusively for JSON-RPC framing.
  std::fprintf(stderr, "wiki-mcp: %s\n", wikicore::versionString());

  const std::string configPath =
      std::filesystem::exists("config.toml") ? "config.toml" : "config.example.toml";
  const wikicore::config::AppConfig cfg = wikicore::config::AppConfig::load(configPath);

  wikicore::index::Database db(cfg.dbPath);
  db.migrate();

  wikicore::vault::VaultRepository vault(cfg.vaultPath);
  wikicore::index::IndexUpdater indexUpdater(db);
  wikicore::vault::DocumentService documents(vault, indexUpdater);
  wikicore::index::FtsSearch search(db);
  wikicore::index::NavQueries nav(db);

  // "admin" (the AppConfig default) sees public+private; only an exact
  // "public" restricts it — this is the opposite direction from the HTTP
  // fail-safe-private rule on purpose: stdio is spawned locally by
  // whoever already has filesystem access to the vault, a different trust
  // boundary than an anonymous web visitor. See docs/architecture.md.
  const bool includePrivate = cfg.mcpScope != "public";

  wikicore::mcp::runServer("personal-wiki", wikicore::versionString(), search, nav,
                            indexUpdater, documents, includePrivate);

  return 0;
}
