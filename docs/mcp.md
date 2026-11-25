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
[`docs/docker.md`](docker.md)), the `command` above won't work as-is —
there's no `wiki-mcp` binary sitting on the host filesystem to point at.
Use `docker exec -i <container> wiki-mcp` as the `command` instead; see
[`docs/docker.md`](docker.md)'s "`wiki-mcp` from a container" section
for the full config shape.

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
- **get_document**(id_or_path) — the full document body plus metadata,
  including its direct `[[wiki-link]]` neighbors (`Links`: outgoing,
  `Backlinks`: other documents linking to this one — one hop only, so a
  client follows one with another `get_document` call rather than
  getting a pre-walked subgraph). Accepts either a vault-relative path
  (`notes/foo.md`) or a document `id` (a UUID), resolved through the
  index either way.
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
  [`docs/query-blocks.md`](query-blocks.md) for the full grammar). Pass
  just the block body, not the surrounding ` ``` ` fences. This runs
  through the exact same `QueryBlocks::parseAndRun` engine as
  `GET /api/query` and the in-app Draft/Chat agent's own
  `run_query_block` tool — one whitelisted DSL, one implementation,
  identical behavior everywhere it's exposed. A typo in a key comes
  back as an actual tool error, never a silent empty
  result.
- **get_calendar_events**(start, end, folder?, tags?) — calendar events
  (documents with a `due` date) within an inclusive date range, using the
  same engine as `/calendar` and `GET /api/calendar` (see
  [`docs/calendar.md`](calendar.md)). Recurring series come back
  already expanded into concrete per-day occurrences — one row per
  occurrence, not one per document. This is how an agent can answer
  "what's due today" or "what's due this week" without reading every
  document in the vault and parsing
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

**Skill documents are read-only to every model-facing write path.** A
document whose front-matter `type` is `skill` (ignoring case and
surrounding whitespace) is instructions that MCP clients may load into
later sessions, so `create_document` and `update_document` refuse to
produce one, `update_document` refuses to touch one, and `attach_file`,
`attach_file_begin` and the `PUT /mcp/uploads/{id}` follow-up refuse to
attach to one; the upload re-checks at `PUT` time, so a document that
became a skill after `attach_file_begin` is refused with `403`. The same
applies to `create_document` in the remote agent endpoint. Each refusal
returns `skill documents are read-only over MCP` and is audit-logged as a
failure. The web UI (admin session) is the only way to write a skill
document. The check is `vault::rejectSkillType` in
`src/vault/SkillDocuments.h`.

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

## Protocol revisions

Both transports serve two MCP revisions side by side, chosen per request:

- **2025-03-26** (`initialize` handshake). An `initialize` request, or
  any request without per-request metadata, gets this behavior,
  unchanged from earlier releases.
- **2026-07-28** (stateless). A request whose `params._meta` carries
  `io.modelcontextprotocol/protocolVersion` is served under this
  revision: `server/discover`, `tools/list`, `tools/call`, and the skills
  methods below (`skills/list`, `skills/get`, `resources/list`,
  `resources/read`). Every request
  must carry `io.modelcontextprotocol/protocolVersion` and
  `io.modelcontextprotocol/clientCapabilities` in `_meta`, otherwise
  `-32602`; a version other than `2026-07-28` gets `-32022` with
  `data.supported`. Every result carries `resultType: "complete"` and
  `io.modelcontextprotocol/serverInfo` in its `_meta`; `server/discover`
  and `tools/list` add `ttlMs` and `cacheScope: "public"`. `ping` does
  not exist in this revision (`-32601`).

Remote HTTP only, 2026-07-28 requests only:

- `MCP-Protocol-Version`, `Mcp-Method` and, for `tools/call`, `Mcp-Name`
  must be present and equal the body values (`Mcp-Name` may use the
  `=?base64?...?=` form); otherwise `400` with `-32020`.
- A request whose `MCP-Protocol-Version` header names a revision newer
  than 2025-11-25 is a 2026-07-28 request even without body `_meta`, so
  it fails with `400` / `-32602` instead of being answered under
  2025-03-26 rules.
- `-32602` for missing `_meta` fields and `-32022` are `400`; an unknown
  method is `404` with `-32601`; a notification is `202` with no body.
- An `Origin` header that does not name the request's `Host` gets `403`.
  When `Host` carries no port (nginx's `proxy_set_header Host $host`
  drops it), only the host names are compared, so a page on any port of
  the same host name passes this check; the bearer token is still
  required. Clients that send no `Origin` (non-browser clients, the
  claude.ai connector) are unaffected.
- `GET` and `DELETE` on `/mcp` answer `405`.

The rules live in `src/core/McpProtocol.h`, shared by both transports.

## Skills

On the 2026-07-28 path both transports serve the MCP skills extension
(`io.modelcontextprotocol/skills`, SEP-2640): `server/discover` declares
`resources` and the extension, and documents with front-matter
`type: skill` become Agent Skills that a client such as Claude Code can
load on demand. The 2025-03-26 path does not serve them.

A document is served as a skill when:

- its `type` is `skill` (ignoring case and surrounding whitespace, the
  same check as the write guard above);
- its file name gives a valid skill name: the basename without `.md`,
  lowercased, `_` turned into `-`, repeated `-` collapsed, leading and
  trailing `-` dropped, and then only `a-z`, `0-9` and `-`, at most 64
  characters (`gpg_sh.md` → `gpg-sh`; a name with a space or non-ASCII
  letters is not served);
- it has a non-empty `description` of at most 1024 characters (what the
  skill does and when to use it; the client chooses skills by it);
- no other skill document gives the same name (if two do, neither is
  served);
- with its attachments it stays within 512 files and 16 MiB;
- it is visible at the transport's scope (`[mcp].scope` for stdio, admin
  for remote), checked against the document's own front matter.

Each skill is `skill://<name>/SKILL.md` plus `skill://<name>/assets/<file>`
for every file in the document's `.assets/` folder. `SKILL.md` is
generated: a YAML block with exactly `name` and `description`
(double-quoted), a `# <title>` heading unless the body already starts with
one, and the body, with links into the document's own `.assets/` folder
rewritten to `assets/<file>` (every form the app writes: `<stem>.assets/`,
`<vault path>.assets/`, `assets/<vault path>.assets/` and a path-absolute
`/.../assets/<vault path>.assets/`, optionally after `./`; links with a
scheme or host, including the wiki's own full URLs, are left alone).
`skills/list` and `skills/get` return each
skill's frontmatter and the size and `sha256:` digest of every file;
`resources/read` returns `SKILL.md` and text attachments as `text`, other
files as base64 `blob`. Unknown skills, files and cursors are `-32602`.
Results carry `ttlMs: 60000` and `cacheScope` `private` whenever they
include a private skill.

In the editor, the Description field edits `description`; for
`type: skill` a line next to it says whether the saved document is
served, or why not.
`GET /api/admin/skills` (admin session) returns the same: the served
skills with their names, and every skipped `type: skill` document with
its reason.

The catalog is `vault::SkillCatalog` (`src/vault/SkillCatalog.h`); the
wire shapes are built once in `src/core/McpSkills.h` for both transports.

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
  excluded from backups. The server's 100 MiB request-body limit and the
  reverse proxy's own `client_max_body_size` bound the upload.
- **Bearer tokens** — the Account page's "Tokens" list holds any number of
  independently labeled tokens (one per client is the intended use: Claude
  Desktop, a Claude Code remote connection, a standalone client, ...), each
  a 64-character random hex string checked via `Authorization: Bearer
  <token>` on every `POST /mcp`. Only a token's SHA-256 hash is ever
  stored — the raw value is never written to disk anywhere, and the list
  shows only a cosmetic 8-character prefix of it to tell tokens apart.
  "Create token" and "Regenerate" each show the new raw value exactly
  once, right there on the page. Regenerating or deleting a token affects
  only that one token — every other token, and the global enabled/write/IP
  allowlist settings, are untouched. `PUT /mcp/uploads/{id}` doesn't send
  this header at all — the upload id itself is the capability.
- **IP allowlist** — optional, and additional to the token rather than a
  replacement for it. Leaving it empty means no IP restriction at all. It
  accepts IPv4 and IPv6, either as a CIDR range or a bare address
  (treated as `/32` or `/128` respectively) — see `src/auth/CidrMatch.h`.
  It is checked before the token: a caller outside the list gets `403`
  whether or not its token is valid.

**This requires TLS somewhere in front of the app.** That can be either
a reverse proxy (see [`docs/sbc-deployment.md`](sbc-deployment.md)'s
"Remote MCP" section for the exact nginx directives needed — including
the ones the IP allowlist depends on to see the real client address
rather than the proxy's own) or `wiki-server`'s own standalone TLS mode
([`docs/sbc-deployment.md`](sbc-deployment.md)'s "Standalone TLS"
section — in that mode the IP allowlist sees the real client address
directly, with no proxy header involved at all). The
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
        "Authorization": "Bearer <token created for this client on the Account page>"
      }
    }
  }
}
```

Create a separate, distinctly labeled token per client rather than sharing
one across several config files — then revoking or regenerating one
client's access (lost laptop, retired integration) never affects any
other client. The token sits in this file as plain text. It's not a
secret baked into the repo, but it is still a live credential — keep this
file out of version control, and readable only by whoever is meant to
run this client. Regenerating or deleting that specific token on the
Account page invalidates whatever is pasted here, so update the file to
match whenever you do that.

**Every write through the remote tools is recorded in the same
`mcp_audit_log` table the local stdio write tools use** (see "Write
tools" above), with tool names prefixed `"remote:"`
(`remote:create_document`, `remote:update_document`,
`remote:attach_file`, `remote:attach_file_begin`). Each entry also carries
the label of whichever token authorized it, so an admin reviewing
`GET /api/admin/mcp-audit-log` can tell not just local from remote writes
but which specific client made a given call.

The remote endpoint is a single stateless `POST /mcp`: every tool call
is independently authenticated, with nothing carried across requests.

## Remote agent endpoint

`POST /mcp/agent` (`src/controllers/RemoteAgentRoutes.cpp`,
`src/llm/RemoteAgentRuntime.cpp`) is a second, deliberately separate endpoint
on the same bearer-token transport. Where `POST /mcp` has the caller name
the exact tool and arguments, this one hands a short conversation to the
wiki's own configured `[llm]` and lets the model itself decide what to do —
the intended use is a client that just wants to ask a question about the
wiki, or say "turn this clipboard text into a document," without running
its own LLM client or holding its own API key.

Tools available to the model: `search_documents`, `list_documents`,
`list_tags`, `get_document`, `get_calendar_events` (read), and
`create_document` (the only write tool). Deliberately NOT available, even
though a token might already have `writeEnabled`: `update_document` and
`attach_file` — letting a model silently rewrite or overwrite EXISTING
content from free text is a materially bigger mistake-blast-radius than
creating something new, independent of whether anyone is actually trying
to abuse it. `get_document` here also omits the link/backlink data
AgentRuntime's own version includes — a detail that isn't needed to
answer a question or avoid a duplicate.

Request body: `{"messages": [{"role": "user" | "assistant", "content":
"..."}, ...]}` — the caller's own plain conversational turns so far (no
`system`/`tool` roles; those are this endpoint's own internal bookkeeping
for one call and are never echoed back). Response:
`{"reply": "...", "documentsCreated": ["path", ...]}` — `documentsCreated`
is empty unless `create_document` actually ran this turn. The model may ask
one clarifying question instead of creating anything immediately (e.g. "public
or private?"); the client answers by sending the whole conversation again
with that answer appended as a `user` turn — the server has no memory of
its own between calls.

**This is gated on top of, not instead of, everything `POST /mcp` already
requires** (enabled, rate limit, IP allowlist, bearer token, write access) —
plus a second, token-specific opt-in: each token's own **Agent access**
checkbox on the Account page (default off, independent of a token already
having ordinary create/update/attach access). The reasoning: naming a tool
directly and letting a model decide what to write from free text are not the
same risk, even on an already-trusted token, so widening one never silently
widens the other. If no LLM is configured on this server at all
(`[llm].provider = "none"`), a permitted call still gets a `503`, never a
`200` with an empty reply.

Every tool call through this endpoint — reads included, not just
`create_document` — is recorded in the same `mcp_audit_log` table as
everything else, tool name prefixed `remote-agent:` (distinct from
`remote:`, which is reserved for a directly-named tool call on `POST
/mcp`). Reads are audited here even though they aren't on plain `POST /mcp`
(where only the write tools are) — the point of this table for an
autonomous agent is seeing what it looked at on the way to a decision, not
just the decision itself. The same per-token label attribution applies.
