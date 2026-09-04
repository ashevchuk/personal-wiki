# MCP server

`wiki-mcp` is a separate binary, stdio transport (JSON-RPC 2.0), spawned directly by an
MCP client (Claude Desktop, Claude Code). Read-only tools are always available;
Phase 2 added two write tools, gated behind `[mcp].write_access` (default **off** — see
"Write tools" below) so an MCP client writing to the vault is a conscious opt-in, not a
silent capability that showed up on the next rebuild.

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

## Write tools (Phase 2, off by default)

Set `write_access = true` under `[mcp]` in `config.toml` to expose two more tools:

- **create_document**(path: string, title?: string, body?: string, type?: string,
  visibility?: string, tags?: string[]) — fails if a document already exists at
  `path`. `visibility` defaults to `"private"` (fail-safe, same as the HTTP create
  route) if omitted.
- **update_document**(path: string, title?: string, body?: string, type?: string,
  visibility?: string, tags?: string[]) — a genuine PARTIAL update: any field left out
  keeps its current value, unlike the HTTP `PUT` route (which always replaces every
  field). Fails if `path` doesn't exist yet.

Both go through the exact same `DocumentService::create`/`update` the HTTP API uses —
same validation, same path-traversal rejection, same atomic write, same index sync,
and (for `update_document`) the same pre-edit snapshot `document_snapshots` records for
every other save (see the "Versioning" section below) — an MCP-driven edit is undoable
through `/history/{path}` exactly like a human-made one.

When `write_access` is `false` (the default), these two tools are not registered at
all — absent from `tools/list`, not present-and-erroring. An MCP client asking "what
can you do" never learns they exist unless the admin opted in.

**Every call through either tool is recorded in the `mcp_audit_log` SQLite table,
success or failure alike** — this is the accountability half of turning `write_access`
on: an LLM writing to your vault unsupervised is a different risk than it reading from
one, and the log is what lets you find out what it actually did, after the fact.
Review it via `GET /api/admin/mcp-audit-log` (admin session required, same as any other
`/api/admin/*` route) — newest first, capped at 200 rows. The table itself persists
regardless of `write_access`'s current value: turning it off after some writes already
happened doesn't erase the record of what was written while it was on.

## Versioning

Every `DocumentService::update` (HTTP `PUT` or the MCP `update_document` tool alike)
snapshots the document's PRE-edit content into `document_snapshots` before overwriting
it — `create`/`create_document` snapshot nothing (there's no "before" state for a
brand-new document). The web UI's `/history/{path}` page lists every past version for a
document, diffs any of them against the current live content (a small client-side
LCS line diff — see `static/js/diff.js`, no vendored diff library), and can restore
one — which itself snapshots the pre-restore state first, so restoring is undoable the
same way any other edit is. No MCP tool exposes this directly (Phase 2's own scope
stopped at write access to the current document); browse history through the web UI.

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

That earlier pass apparently never surfaced the framing quirk documented right below —
almost certainly because it matched responses by their `"id"` field rather than
assuming one line in, one line out in strict order, which is exactly what papers over
this.

## Known issue: an extra blank `{}` line after `notifications/initialized` (vendored library, not our code)

Confirmed 2026-09-04 by driving a real JSON-RPC handshake into the actual prod
`wiki-mcp` binary over stdin/stdout by hand: right after the client sends the
mandatory `notifications/initialized` notification (part of every spec-compliant MCP
handshake, sent before any real tool call), the vendored `hkr04/cpp-mcp`'s
`server::start_stdio()` prints a spurious, empty `{}\n` to stdout — a line with no
`id`, no `result`, no `error`, which is not a valid JSON-RPC 2.0 message at all (a
notification is defined as receiving **no** response, ever). Every following response
shows up one line "late" behind it as a result — read one line per request the naive
way (as this project's own now-doesn't-exist `/tmp/mcp_e2e_test.py` apparently didn't),
and `tools/list`'s response reads back as `{}` while the actual tool list sits one
`readline()` further down, still unread.

Root cause, read directly in the vendored source
(`mcp_server.cpp::process_request()` / `start_stdio()`): a notification is handled by

```cpp
if (req.is_notification()) {
    if (req.method == "notifications/initialized") set_session_initialized(session_id, true);
    return json::object();  // {} — NOT json(nullptr)
}
```

and `start_stdio()` decides whether to print with

```cpp
if (!res.is_null() && !req.id.is_null()) {
    std::cout << res.dump() << "\n" << std::flush;
} else {
    std::cerr << "Response is null or ID is null. Method: " << req.method << std::endl;
    if (!res.is_null()) std::cout << res.dump() << "\n" << std::flush;  // <-- fires
}
```

`json::object()` (an empty object) and `json(nullptr)` (JSON `null`) are different
nlohmann::json values — `{}.is_null()` is `false` — so the `else` branch's own
`if (!res.is_null())` guard, meant to be a fallback for something else, ends up
matching every notification and prints it anyway.

**Not our bug to fix in `libwikicore`/`src/mcp/` — this lives entirely inside the
vendored, `FetchContent`-pinned `cpp-mcp` dependency** (see "Implementation" above).
Not patched here: the practical blast radius looks small (a spec-conformant client
reads the STDIO transport by matching each response's `id` against its own table of
pending requests, per the MCP/JSON-RPC 2.0 spec — a stray `id`-less, non-conforming
line has nothing to match and is expected to be ignored, not treated as fatal); no
live Claude Desktop/Code session in this dev environment to confirm that empirically
against the ACTUAL client rather than the spec's own description of correct client
behavior. Revisit if a real MCP client session ever visibly chokes on this — the fix,
if it's ever done, is either patching the vendored source post-fetch (a `PATCH_COMMAND`
on the `FetchContent_Declare` call) or filing it upstream against `hkr04/cpp-mcp`.
