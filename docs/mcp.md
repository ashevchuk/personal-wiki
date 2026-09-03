# MCP server

`wiki-mcp` is a separate binary, stdio transport (JSON-RPC 2.0), spawned directly by an
MCP client (Claude Desktop, Claude Code). Read-only in the MVP: no write tools at all.

## Connecting from Claude Desktop

Add to `claude_desktop_config.json` (macOS: `~/Library/Application Support/Claude/`,
Linux: `~/.config/Claude/`):

```json
{
  "mcpServers": {
    "personal-wiki": {
      "command": "/opt/wiki/bin/wiki-mcp",
      "args": [],
      "cwd": "/opt/wiki"
    }
  }
}
```

`cwd` must point at the directory holding `config.toml` (the same one `wiki-server`
uses) — `wiki-mcp` looks for it relative to the current working directory, same as
`wiki-server`. If `config.toml` is missing, it falls back to `config.example.toml`
(defaults).

**Important**: `wiki-mcp` does NOT rescan the vault on every start (unlike
`wiki-server`) — a full walk on every MCP client spawn would contradict the
"starts instantly" requirement. It trusts the existing index. Make sure `wiki-server`
has run at least once (it rescans at startup) or run `wiki-server --reindex` manually
before the first MCP client connection.

## Tools (read-only)

- **search_documents**(query: string, tags?: string[], type?: string, limit?: number) —
  full-text search (FTS5, bm25 ranking), a snippet with matches highlighted
  (`**term**`, markdown bold — not HTML, an MCP client reads text, it doesn't render a
  page).
- **get_document**(id_or_path: string) — the full document body + metadata. Accepts
  either a vault-relative path (`notes/foo.md`) or an `id` (uuid) — resolved via the
  index.
- **list_tags**() — every tag with its document count.
- **list_documents**(tag?: string, type?: string, folder?: string, limit?: number,
  offset?: number) — browsing without a search query, with pagination. `folder` is a
  path prefix (`"notes/"` matches `notes/foo.md`, `notes/sub/bar.md`).

All four are visibility-aware: a private document never appears in any result unless
`[mcp].scope` in `config.toml` is `"admin"` (the default). `scope = "public"` restricts
the MCP client to public content only — the same fail-safe-private principle as the
HTTP layer, applied to a different trust boundary (a local process spawn by the
machine's owner, not an anonymous web visit).

## Implementation

- Protocol layer: [hkr04/cpp-mcp](https://github.com/hkr04/cpp-mcp), vendored via CMake
  `FetchContent`, pinned to a specific commit (no vcpkg port exists). The spike against
  GCC 16.2.1/C++20 compiled clean on the first try — no fallback to a hand-rolled
  JSON-RPC was needed (compared to the plan, where it was listed as a backup option).
- `src/mcp/McpServer.cpp` — registers the 4 tools + wraps `index::FtsSearch`,
  `index::NavQueries`, `index::IndexUpdater::findPathByUuid`,
  `vault::DocumentService::get` — all of this already exists in `libwikicore` from
  M2/M3, the MCP layer only translates results into `mcp::json`.
- `mcp::json` = `nlohmann::ordered_json`, vendored as a separate copy inside cpp-mcp
  (`common/json.hpp`) — NOT the same `nlohmann_json` the rest of the project gets from
  vcpkg. `McpServer.cpp` deliberately never includes both in the same single
  compilation unit (ODR risk with two copies of the same header).

## Verification

No live Claude Desktop exists in the dev environment (a headless sandbox, no GUI) — so
E2E verification is a scripted JSON-RPC client (`/tmp/mcp_e2e_test.py` during
development, not in the repo) that drives real MCP messages (`initialize` →
`notifications/initialized` → `tools/list` → `tools/call` ×N) into a real `wiki-mcp`
process over stdin/stdout and checks the responses. Run twice against the same index —
with `scope=admin` and `scope=public` — 17/17 checks passing in both modes: all 4 tools
register, search with highlighting works, `get_document` works by both path and uuid,
path traversal and "not found" correctly produce `isError:true` instead of a protocol
crash, and, most importantly, visibility gating actually flips along with scope, not
just "looks connected".
