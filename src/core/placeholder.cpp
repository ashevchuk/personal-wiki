// libwikicore is the vault+index+MCP-tool-logic static library — it is
// deliberately kept free of Drogon/OpenSSL so `wiki-mcp` (the stdio MCP
// binary) can link against it without dragging in the HTTP stack. Real
// content (PathGuard, VaultRepository, SqliteIndex, ...) lives in
// src/vault/ and src/index/; this file exists only so src/core/ itself
// has a translation unit to build.

#include "core/wikicore.h"

#ifndef WIKI_VERSION
#define WIKI_VERSION "0.0.0-dev"
#endif

namespace wikicore {

const char* versionString() {
  return "personal-wiki " WIKI_VERSION;
}

}  // namespace wikicore
