#pragma once

// Public interface of libwikicore (vault + index + MCP tool logic).
// See docs/architecture.md for the module breakdown.

namespace wikicore {

// "personal-wiki X.Y.Z", for --version and the startup log line.
const char* versionString();
// Bare "X.Y.Z", for MCP serverInfo.version (the name has its own field).
const char* versionNumber();

}  // namespace wikicore
