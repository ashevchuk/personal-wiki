# MCP server

`wiki-mcp` is a separate binary that speaks stdio JSON-RPC 2.0, spawned
directly by an MCP client (Claude Desktop, Claude Code) running on the
*same* machine as the vault. Read-only tools are always available; write
tools were added later, gated behind `[mcp].write_access` (default
**off** — see "Write tools" below), so letting an MCP client write to
the vault is a conscious opt-in, not a capability that silently shows up
on the next rebuild.

A separate feature, the remote (HTTP) transport, lets an MCP client
reach the same tools over the network instead of through a local spawn —
see "Remote (HTTP) transport" below. The two are independent of each
other: enabling remote access doesn't touch `wiki-mcp`/stdio at all, and
turning on stdio write access doesn't affect the remote transport either.

## Connecting from Claude Desktop

Add this to `claude_desktop_config.json` (macOS:
`~/Library/Application Support/Claude/`, Linux: `~/.config/Claude/`):

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

`cwd` must point at the directory holding `config.toml` — the same one
`wiki-server` uses. `wiki-mcp` looks for that file relative to its
current working directory, just like `wiki-server` does. If
`config.toml` is missing there, it falls back to `config.example.toml`'s
defaults.

If you're running the whole app in Docker instead of natively (see
`docs/docker.md`), the `command` above won't work as-is — there's no
`wiki-mcp` binary sitting on the host filesystem to point at. Use
`docker exec -i <container> wiki-mcp` as the `command` instead; see
`docs/docker.md`'s "`wiki-mcp` from a container" section for the full
config shape.

**Important:** unlike `wiki-server`, `wiki-mcp` does **not** rescan the
vault on every start — a full walk on every MCP client spawn would break
the "starts instantly" expectation. It simply trusts whatever index
already exists. Make sure `wiki-server` has run at least once (it
rescans at startup), or run `wiki-server --reindex` manually, before the
first MCP client connection.

## Tools (read-only)

- **search_documents**(query, tags?, type?, limit?) — full-text search
  (FTS5, BM25 ranking), returning a snippet with matches highlighted as
  `**term**` (markdown bold, not HTML — an MCP client reads text, it
  doesn't render a page).
- **get_document**(id_or_path) — the full document body plus metadata.
  Accepts either a vault-relative path (`notes/foo.md`) or a document
  `id` (a UUID), resolved through the index either way.
- **list_tags**() — every tag with its document count, e.g.
  `[{"tag": "lang/cpp", "count": 3}, {"tag": "meta", "count": 12}, ...]`.
  A `/` inside a tag has no special meaning to this tool, or to anything
  server-side — it's just a plain character in the tag string. It *is* a
  convention the web UI understands purely on the client side: a tag
  containing `/` (e.g. `lang/cpp`, `lang/python`, `project/wiki`) renders
  grouped into a collapsible namespace tree in the sidebar, with a filter
  box above it, instead of showing up in one flat list. Worth knowing if
  an MCP client is the one choosing tags for `create_document`/
  `update_document` below — picking `lang/cpp` over a bare `cpp` costs
  nothing and gets free grouping in the UI a human later browses.
- **list_documents**(tag?, type?, folder?, limit?, offset?) — browsing
  without a search query, with pagination. `folder` is a path prefix
  (`"notes/"` matches both `notes/foo.md` and `notes/sub/bar.md`).
- **run_query**(query) — executes a ` ```query ` fenced block's body
  (the `key: value` DSL — `type`, `tag`, `folder`, `search`, `sort`,
  `order`, `limit`, `orphans`, `todos`, `links`; see
  `docs/query-blocks.md` for the full grammar). Pass just the block body,
  not the surrounding ` ``` ` fences. This runs through the exact same
  `QueryBlocks::parseAndRun` engine as `GET /api/query` and the in-app
  Draft/Chat agent's own `run_query_block` tool — one whitelisted DSL,
  one implementation, identical behavior everywhere it's exposed. A typo
  in a key comes back as an actual tool error, never a silent empty
  result.
- **get_calendar_events**(start, end, folder?, tags?) — calendar events
  (documents with a `due` date) within an inclusive date range, using the
  same engine as `/calendar` and `GET /api/calendar` (see
  `docs/calendar.md`). Recurring series come back already expanded into
  concrete per-day occurrences — one row per occurrence, not one per
  document. This is how an agent can answer "what's due today" or "what's
  due this week" without reading every document in the vault and parsing
  front matter itself.

All six tools are visibility-aware: a private document never appears in
any result unless `[mcp].scope` in `config.toml` is `"admin"` (the
default). Setting `scope = "public"` restricts the MCP client to public
content only — the same fail-safe-private principle the HTTP layer
follows, applied to a different trust boundary (a local process the
machine's owner chose to spawn, rather than an anonymous web visit).

## Write tools (off by default)

Set `write_access = true` under `[mcp]` in `config.toml` to expose three
more tools:

- **create_document**(path, title?, body?, type?, visibility?, tags?) —
  fails if a document already exists at `path`. A path that doesn't
  already end in `.md` gets `.md` appended automatically (the same thing
  the HTTP create route does — `IndexBuilder` only ever walks `.md`
  files). `visibility` defaults to `"private"` if omitted, the same
  fail-safe default the HTTP create route uses.
- **update_document**(path, title?, body?, type?, visibility?, tags?) —
  a genuine *partial* update: any field you leave out keeps its current
  value. This differs from the HTTP `PUT` route, which always replaces
  every field. It fails if `path` doesn't exist yet.
- **attach_file**(path, filename?, source_path?, content_base64?) —
  stores the file in the owning document's co-located `<stem>.assets/`
  folder, using the same `AttachmentService` the web UI's Attach button
  uses (sanitized filename, de-duped on collision, **no size cap**), and
  appends a markdown link to the document body. Raster images that
  `GET /assets/` serves inline (`png`/`jpg`/`gif`/`webp`) become
  `![name](assets/...)`; everything else becomes `[name](assets/...)`.
  The resulting href is relative to `<base href="{basePath}/">`, the same
  convention wiki-links use.

  Pass **exactly one** of:
  - **source_path** — an absolute path to a file on the machine running
    `wiki-mcp`. The bytes are copied straight from disk
    (`std::filesystem::copy_file`), never routed through the model — this
    is how a 120 MB PDF gets attached without choking on tokens.
    `filename` defaults to the source file's basename. This option is
    **stdio only**: the remote HTTP transport rejects `source_path`
    outright, since copying an arbitrary path would let a remote caller
    read arbitrary files off the wiki host. Use `attach_file_begin` +
    `PUT /mcp/uploads/{id}` there instead (see "Remote (HTTP) transport"
    below).
  - **content_base64** — the file's bytes, as standard or URL-safe
    base64 (a `data:` URL prefix is stripped automatically if present).
    This is only meant for small files — the encoded string is capped
    around 36 MiB so a single huge JSON tool call can't balloon into a
    multi-hundred-MB `std::string`.

  The owning document must already exist; there's no "attach to a
  document that hasn't been saved yet," matching the HTTP upload route's
  own behavior.

`create_document`/`update_document` go through the exact same
`DocumentService::create`/`update` calls the HTTP API itself uses — same
validation, same path-traversal rejection, same atomic write, same index
sync. For `update_document`, and for `attach_file` (which calls `update`
internally to append the link), that also means the same pre-edit
snapshot into `document_snapshots` that every other save gets (see
"Versioning" below) — an MCP-driven edit is just as undoable through
`/history/{path}` as a human-made one.

When `write_access` is `false` (the default), these three tools aren't
registered at all — they're absent from `tools/list`, not present and
simply erroring when called. An MCP client asking "what can you do" never
even learns they exist unless the admin has opted in.

**Every call through any of these tools is recorded in the
`mcp_audit_log` SQLite table, whether it succeeds or fails.** This is the
accountability half of turning `write_access` on: an LLM writing to your
vault unsupervised carries a different risk than it reading from one, and
this log is what lets you find out, after the fact, what it actually
did. Review it via `GET /api/admin/mcp-audit-log` (requires an admin
session, same as any other `/api/admin/*` route) — newest first, capped
at 200 rows. The table itself persists regardless of `write_access`'s
current value: turning the setting back off after some writes already
happened doesn't erase the record of what was written while it was on.

## Versioning

Every `DocumentService::update` call — whether triggered by the HTTP
`PUT` route, the MCP `update_document` tool, or `attach_file` appending
a link — snapshots the document's *pre-edit* content into
`document_snapshots` before overwriting it. `create`/`create_document`
snapshot nothing, since there's no "before" state for a brand-new
document.

The web UI's `/history/{path}` page lists every past version of a
document, can diff any of them against the current live content (a
small client-side line diff — see `static/js/diff.js`, no vendored diff
library involved), and can restore an older version. Restoring itself
snapshots the pre-restore state first, so a restore is just as undoable
as any other edit. No MCP tool currently exposes history browsing
directly — that stayed in scope for the web UI only when write access
was first added; browse history there instead.

## Remote (HTTP) transport

This is a separate, admin-toggleable `POST /mcp` route inside
`wiki-server` itself (`src/controllers/RemoteMcpRoutes.cpp`), letting an
MCP client reach this wiki's tools over HTTPS from anywhere — not just
through a local stdio spawn. It's off by default, and every setting
below is managed live from the Account page, backed by SQLite
(`McpRemoteConfig`) rather than `config.toml` — the whole point is being
able to toggle this without restarting the server:

- **Enabled** — the master switch. While it's off, `POST /mcp` and
  `PUT /mcp/uploads/{id}` both answer a plain `404` to everyone,
  indistinguishable from a route that was never registered at all —
  never a `401` that would hint "this exists, bring a token."
- **Write access** — independent of `[mcp].write_access` above, which
  only controls the *local* stdio server. Allowing
  `create_document`/`update_document`/`attach_file` from the open
  internet is a much bigger step than allowing it from a local process
  only your own machine can spawn, so it gets its own explicit opt-in
  rather than inheriting the local setting.

  Remote write access also adds **attach_file_begin**, which isn't
  registered on stdio at all: it returns an `upload_id`, and the client
  then `PUT`s the raw file bytes to `/mcp/uploads/{upload_id}` on the
  same origin. That follow-up request does **not** send the Bearer
  token — the UUID itself is a one-hour capability secret, so an agent
  can `curl -T file.pdf` without ever pasting the token into shell
  history, and the file's bytes never enter the model's context at all.
  `source_path` is rejected on this transport, since it would let a
  remote caller read arbitrary files off the wiki host. A multipart
  `POST` with a file field also works as an alternative. Upload tickets
  live in `<vault>/.mcp-uploads/` — a dotdir, skipped by the indexer and
  excluded from backups. Drogon's `setClientMaxBodySize` (2 GiB) and the
  reverse proxy's own `client_max_body_size` still bound the HTTP
  request; the app itself has no attachment size cap of its own.
- **Bearer token** — a 64-character random hex string, checked via
  `Authorization: Bearer <token>` on every `POST /mcp`. Only its SHA-256
  hash is ever stored — the raw value is never written to disk anywhere.
  Clicking "Regenerate token" shows the new raw
  value exactly once, right there on the page, and immediately
  invalidates whatever token was issued before it. `PUT /mcp/uploads/{id}`
  doesn't send this header at all — the upload id itself is the
  capability.
- **IP allowlist** — optional, and additional to the token rather than a
  replacement for it. Leaving it empty means no IP restriction at all. It
  accepts IPv4 and IPv6, either as a CIDR range or a bare address
  (treated as `/32` or `/128` respectively) — see `src/auth/CidrMatch.h`.

**This requires TLS somewhere in front of the app.** That can be either
a reverse proxy (see `docs/sbc-deployment.md`'s "Remote MCP" section for
the exact nginx directives needed — including the ones the IP allowlist
depends on to see the real client address rather than the proxy's own)
or `wiki-server`'s own standalone TLS mode (`docs/sbc-deployment.md`'s
"Standalone TLS" section — in that mode the IP allowlist sees the real
client address directly, with no proxy header involved at all). The
bearer token travels in a plain header, so without TLS somewhere between
the client and this app, it's readable by anything sitting in between.

### Connecting a client

Claude Code/Desktop's `mcpServers` config supports an HTTP transport with
a static header, in the same config file as the stdio example above:

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

The token sits in this file as plain text. It's not a secret baked into
the repo, but it is still a live credential — keep this file out of
version control, and readable only by whoever is meant to run this
client. Regenerating the token on the Account page invalidates whatever
is pasted here, so update the file to match whenever you do that.

**Every write through the remote tools is recorded in the same
`mcp_audit_log` table the local stdio write tools use** (see "Write
tools" above), with tool names prefixed `"remote:"`
(`remote:create_document`, `remote:update_document`,
`remote:attach_file`, `remote:attach_file_begin`). That lets an admin
reviewing `GET /api/admin/mcp-audit-log` tell local and remote writes
apart at a glance — it needed no schema change, just a naming convention
at the call site.

The remote endpoint is a single stateless `POST /mcp`: every tool call
is independently authenticated, with nothing carried across requests.
