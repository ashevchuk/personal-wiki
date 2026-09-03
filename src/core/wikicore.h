#pragma once

// Public interface of libwikicore (vault + index + MCP tool logic).
// Milestone 0: bootstrap stub only. See docs/architecture.md for the
// module breakdown landing in M1–M4.

namespace wikicore {

// Sanity symbol used by wiki-server/wiki-mcp at startup to confirm the
// static library actually linked in — removed once real modules exist.
const char* versionString();

}  // namespace wikicore
