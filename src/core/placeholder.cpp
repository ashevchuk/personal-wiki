// libwikicore is the vault+index+MCP-tool-logic static library — it is
// deliberately kept free of Drogon/OpenSSL so `wiki-mcp` (the stdio MCP
// binary) can link against it without dragging in the HTTP stack. Real
// content (PathGuard, VaultRepository, SqliteIndex, ...) lives in
// src/vault/ and src/index/; this file exists only so src/core/ itself
// has a translation unit to build.

#include "core/wikicore.h"

namespace wikicore {

const char* versionString() {
  return "personal-wiki core 0.1.0";
}

}  // namespace wikicore
