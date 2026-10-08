# MCP server

`wiki-mcp` is a separate binary, stdio transport (JSON-RPC 2.0), spawned directly by an
MCP client (Claude Desktop, Claude Code) on the SAME machine as the vault. Read-only
tools are always available; Phase 2 added write tools, gated behind
`[mcp].write_access` (default **off** — see "Write tools" below) so an MCP client
writing to the vault is a conscious opt-in, not a silent capability that showed up on
the next rebuild.

A SEPARATE Phase 2 feature, the remote (HTTP) transport, lets an MCP client reach the
same tools over the network instead — see "Remote (HTTP) transport" below. The two are
independent: enabling remote access doesn't touch `wiki-mcp`/stdio at all, and vice
versa.

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

Running the whole app in Docker instead (`docs/docker.md`) rather than natively? The
`command` above doesn't apply — there's no `wiki-mcp` binary sitting on the host
filesystem to point at. Use `docker exec -i <container> wiki-mcp` as the `command`
instead; see `docs/docker.md`'s "`wiki-mcp` from a container" section for the full
`claude_desktop_config.json` shape.

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
- **list_tags**() — every tag with its document count, e.g.
  `[{"tag": "lang/cpp", "count": 3}, {"tag": "meta", "count": 12}, ...]`. A `/` inside a
  tag has no special meaning to this tool, or to anything server-side — it's a plain
  character in the tag string, same as any other. It IS a convention the Web UI
  understands purely client-side: a tag containing `/` (e.g. `lang/cpp`, `lang/python`,
  `project/wiki`) renders grouped into a collapsible namespace tree in the sidebar
  (`nav.js::buildTags`) instead of a flat list, with a filter box above it — worth
  knowing if an MCP client is the one CHOOSING tags for `create_document`/
  `update_document` below: picking `lang/cpp` over a bare `cpp` costs nothing and gets
  free grouping in the actual UI a human later browses.
- **list_documents**(tag?: string, type?: string, folder?: string, limit?: number,
  offset?: number) — browsing without a search query, with pagination. `folder` is a
  path prefix (`"notes/"` matches `notes/foo.md`, `notes/sub/bar.md`).
- **run_query**(query: string) — executes a ` ```query ` fenced block's body (the
  `key: value` DSL — `type`, `tag`, `folder`, `search`, `sort`, `order`, `limit`,
  `orphans`, `todos`, `links`; see `docs/architecture.md`'s query-blocks section for the
  full grammar). Pass the block body only, not the surrounding ` ``` ` fences. Same
  `QueryBlocks::parseAndRun` as `GET /api/query` and the in-app Draft/Chat agent's own
  `run_query_block` tool — one whitelisted DSL, one implementation, identical behavior
  everywhere it's exposed. A parse error (typo'd key) comes back as an actual tool
  error, never a silent empty result.
- **get_calendar_events**(start: string, end: string, folder?: string, tags?: string[]) —
  calendar events (documents with a `due` date) in an inclusive date range, same engine
  as `/calendar` and `GET /api/calendar` (`docs/calendar.md`). Recurring series are
  already expanded into concrete per-day occurrences — one row per occurrence, not one
  per document. This is how an agent answers "what's due today/this week" without
  reading every document in the vault and parsing front matter itself.

All six are visibility-aware: a private document never appears in any result unless
`[mcp].scope` in `config.toml` is `"admin"` (the default). `scope = "public"` restricts
the MCP client to public content only — the same fail-safe-private principle as the
HTTP layer, applied to a different trust boundary (a local process spawn by the
machine's owner, not an anonymous web visit).

## Write tools (Phase 2, off by default)

Set `write_access = true` under `[mcp]` in `config.toml` to expose three more tools:

- **create_document**(path: string, title?: string, body?: string, type?: string,
  visibility?: string, tags?: string[]) — fails if a document already exists at
  `path`. A path that doesn't already end in `.md` gets `.md` appended (same as
  the HTTP create route — IndexBuilder only walks `.md` files). `visibility`
  defaults to `"private"` (fail-safe, same as the HTTP create
  route) if omitted.
- **update_document**(path: string, title?: string, body?: string, type?: string,
  visibility?: string, tags?: string[]) — a genuine PARTIAL update: any field left out
  keeps its current value, unlike the HTTP `PUT` route (which always replaces every
  field). Fails if `path` doesn't exist yet.
- **attach_file**(path: string, filename?: string, source_path?: string,
  content_base64?: string) — stores the file in the owning document's co-located
  `<stem>.assets/` folder (same `AttachmentService` the web UI's Attach button uses:
  sanitized filename, de-duped on collision, **no size cap**) and appends a markdown
  link to the document body. Raster images that `GET /assets/` will serve inline
  (`png`/`jpg`/`gif`/`webp`) become `![name](assets/...)`; everything else becomes
  `[name](assets/...)`. The href is relative to `<base href="{basePath}/">`, same
  convention as wiki-links.

  Pass **exactly one** of:
  - **source_path** — absolute path of a file on the machine running `wiki-mcp`. The
    bytes are copied from disk (`std::filesystem::copy_file`), never through the
    model. This is how a 120 MB PDF gets attached. `filename` defaults to the source
    basename. **stdio only.** The remote HTTP transport rejects `source_path`
    (copying a path on the wiki host would read arbitrary files off that host) —
    use `attach_file_begin` + `PUT /mcp/uploads/{id}` there instead (see
    "Remote (HTTP) transport" below).
  - **content_base64** — file bytes (standard or URL-safe base64; a `data:` URL
    prefix is stripped). Only for small files; the encoded string is gated around
    36 MiB so a huge JSON tool-call doesn't become a multi-hundred-MB `std::string`.

  The owning document must already exist — there is no "attach to a document that
  hasn't been saved yet", matching the HTTP upload route.

`create_document`/`update_document` go through the exact same `DocumentService::create`/
`update` the HTTP API uses — same validation, same path-traversal rejection, same
atomic write, same index sync, and (for `update_document` and `attach_file`, which
itself calls `update` to append the link) the same pre-edit snapshot `document_snapshots`
records for every other save (see the "Versioning" section below) — an MCP-driven edit
is undoable through `/history/{path}` exactly like a human-made one.

When `write_access` is `false` (the default), these three tools are not registered at
all — absent from `tools/list`, not present-and-erroring. An MCP client asking "what
can you do" never learns they exist unless the admin opted in.

**Every call through any of these tools is recorded in the `mcp_audit_log` SQLite table,
success or failure alike** — this is the accountability half of turning `write_access`
on: an LLM writing to your vault unsupervised is a different risk than it reading from
one, and the log is what lets you find out what it actually did, after the fact.
Review it via `GET /api/admin/mcp-audit-log` (admin session required, same as any other
`/api/admin/*` route) — newest first, capped at 200 rows. The table itself persists
regardless of `write_access`'s current value: turning it off after some writes already
happened doesn't erase the record of what was written while it was on.

## Versioning

Every `DocumentService::update` (HTTP `PUT`, the MCP `update_document` tool, or
`attach_file` appending a link) snapshots the document's PRE-edit content into
`document_snapshots` before overwriting it — `create`/`create_document` snapshot nothing
(there's no "before" state for a brand-new document). The web UI's `/history/{path}` page lists every past version for a
document, diffs any of them against the current live content (a small client-side
LCS line diff — see `static/js/diff.js`, no vendored diff library), and can restore
one — which itself snapshots the pre-restore state first, so restoring is undoable the
same way any other edit is. No MCP tool exposes this directly (Phase 2's own scope
stopped at write access to the current document); browse history through the web UI.

## Remote (HTTP) transport

Phase 2 — an admin-toggleable `POST /mcp` route in `wiki-server` itself
(`src/controllers/RemoteMcpRoutes.cpp`), letting an MCP client reach this wiki's tools
over HTTPS from anywhere, not just a local stdio spawn. Off by default; every setting
below is managed live from the Account page (SQLite-backed via `McpRemoteConfig`, not
`config.toml` — the whole point is toggling this without a server restart):

- **Enabled** — the master switch. While off, `POST /mcp` and `PUT /mcp/uploads/{id}`
  answer a plain `404` to everyone, indistinguishable from a route that was never
  registered — not a `401` hinting "this exists, bring a token."
- **Write access** — independent of `[mcp].write_access` above (the LOCAL stdio
  server's own flag). Allowing `create_document`/`update_document`/`attach_file` from the open
  internet is a bigger step than allowing it from a local process only your own
  machine can spawn; it gets its own explicit opt-in rather than inheriting the local
  setting. Remote write tools also include **attach_file_begin** (not registered on
  stdio): it returns an `upload_id`, then the client `PUT`s the raw file bytes to
  `/mcp/uploads/{upload_id}` on the same origin. That follow-up request does **not**
  send the Bearer token — the UUID is a one-hour capability secret, so an agent can
  `curl -T file.pdf` without pasting the token into a shell history, and the file
  bytes never enter the model's context. `source_path` is rejected on this transport
  (it would read arbitrary files off the wiki host). Multipart `POST` with a file
  field also works. Tickets live in `<vault>/.mcp-uploads/` (a dotdir, skipped by
  the indexer; excluded from backups). Drogon's `setClientMaxBodySize` (2 GiB) and
  the reverse proxy's `client_max_body_size` still bound the HTTP request; the app
  itself has no attachment size cap.
- **Bearer token** — a 64-char random hex string, checked via `Authorization: Bearer
  <token>` on every `POST /mcp`. Only its SHA-256 hash is ever stored (same discipline as
  session cookies — see `docs/architecture.md`); "Regenerate token" shows the new raw
  value exactly once, right there, and immediately invalidates whatever token was
  issued before. `PUT /mcp/uploads/{id}` does not send this header — the upload id is
  the capability.
- **IP allowlist** — optional, additional to the token, not instead of it. Empty means
  no IP restriction. Accepts IPv4 and IPv6, CIDR or a bare address (treated as `/32` or
  `/128`) — see `src/auth/CidrMatch.h`.

**Requires TLS in front of this app** — either a reverse proxy (see
`docs/sbc-deployment.md`'s "Remote MCP" section for the exact nginx directives this
needs, including the ones the IP allowlist depends on to see the real client address
rather than the proxy's own) or `wiki-server`'s own standalone TLS mode
(`docs/sbc-deployment.md`'s "Standalone TLS" section — in that mode the IP allowlist
sees the real client address directly, with no proxy header involved at all). The
token travels in a plain header; without TLS somewhere between the client and this
app, it's readable by anything in between.

### Connecting a client

Claude Code/Desktop's `mcpServers` config takes an HTTP transport with a static
header, same file as the stdio example above:

```json
{
  "mcpServers": {
    "personal-wiki-remote": {
      "type": "http",
      "url": "https://wiki.example.com/mcp",
      "headers": {
        "Authorization": "Bearer <token from the Account page>"
      }
    }
  }
}
```

The token sits in this file as plain text — not a secret baked into the repo, but
still a live credential; keep the file out of version control and readable only by
whoever is meant to run this client. Regenerating the token on the Account page
invalidates whatever's pasted here; update the file to match.

**Every write through the remote tools is recorded in the same `mcp_audit_log` table
the local stdio write tools use** (see "Write tools" above), with tool names prefixed
`"remote:"` (`remote:create_document`, `remote:update_document`,
`remote:attach_file`, `remote:attach_file_begin`) so an admin reviewing
`GET /api/admin/mcp-audit-log` can tell local and remote writes apart at a glance —
no schema change needed for that, just a naming convention at the call site.

The remote endpoint is a single stateless `POST /mcp` implementing MCP's Streamable
HTTP transport, minus the optional `Mcp-Session-Id` (every tool here is a fast,
synchronous call with nothing to carry across requests, so there's no session state
worth tracking — each request is independently authenticated end to end). Deliberately
hand-built rather than using cpp-mcp's own HTTP+SSE server: reading `mcp_server.cpp`
directly turned up that its `set_auth_handler()` is set but **never actually invoked**
anywhere in that library's request path — an unpatched, dead-code auth hook. Building
on it and trusting that hook for a public endpoint would have shipped something that
*looks* token-protected and isn't. `RemoteMcpRoutes.cpp` reuses only the underlying
`wikicore` services (`FtsSearch`/`NavQueries`/`DocumentService`/`IndexUpdater`) that
`McpServer.cpp` (stdio) also calls, gated by this app's own real, tested Drogon
session-adjacent machinery instead — the stdio transport in `McpServer.cpp` is
untouched by any of this, a completely separate, already-working code path.

## Implementation

- Protocol layer (JSON-RPC 2.0 framing over one-message-per-line stdio, `initialize`,
  `tools/list`, `tools/call`) is hand-rolled in `src/mcp/McpServer.cpp` — no MCP
  library dependency. A JSON-RPC notification (a message with no `id`, e.g.
  `notifications/initialized`) never gets a response, not even an empty one — the
  dispatch loop enforces this generically for any method, not as a case keyed on one
  method name.
- `src/mcp/McpServer.cpp` — registers the tools + wraps `index::FtsSearch`,
  `index::NavQueries`, `index::IndexUpdater::findPathByUuid`,
  `vault::DocumentService::get` — all of this already exists in `libwikicore` from
  M2/M3, the MCP layer only translates results into `mcp::json`.
- `mcp::json` = `nlohmann::ordered_json`, built from this project's own vcpkg
  `nlohmann_json` dependency (already `PUBLIC`-linked into `wikicore`, so `wiki-mcp`
  gets it for free).
- `cpp-mcp` is `FetchContent`-pinned in `CMakeLists.txt` via `FetchContent_Populate`
  (its own `CMakeLists.txt`/`examples/` never get configured or built) purely as a
  source for its vendored, header-only `common/httplib.h`, used by
  `CloudChatClient.cpp` and `CloudEmbeddingProvider.cpp` for outbound HTTPS. Nothing
  links its `mcp` library target.
