// Milestone 0 bootstrap placeholder.
//
// libwikicore is the vault+index+MCP-tool-logic static library — it is
// deliberately kept free of Drogon/OpenSSL so `wiki-mcp` (the stdio MCP
// binary) can link against it without dragging in the HTTP stack. Real
// content (PathGuard, VaultRepository, SqliteIndex, ...) lands starting
// Milestone 1; this file exists only so the target has a translation unit
// to build in M0.

#include "core/wikicore.h"

namespace wikicore {

const char* versionString() {
  return "personal-wiki core 0.1.0 (M0 bootstrap)";
}

}  // namespace wikicore
