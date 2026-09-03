// wiki-mcp — MCP stdio entrypoint.
//
// Deliberately NOT linked against Drogon: Claude Desktop/Code spawn this
// process directly per-session, so it must start instantly and carry no
// HTTP-stack weight. Real implementation (hkr04/cpp-mcp integration or the
// hand-rolled JSON-RPC fallback, plus the 4 read-only tools) lands in
// Milestone 4; see /home/slayer/.claude/plans/zazzy-twirling-sundae.md.

#include <cstdio>

#include "core/wikicore.h"

int main() {
  std::fprintf(stderr, "wiki-mcp: %s (M0 bootstrap, no MCP tools yet)\n",
               wikicore::versionString());
  return 0;
}
