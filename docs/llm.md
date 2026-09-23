# Draft and Chat agents

The edit page can show a **Draft** button that talks to an OpenAI-compatible
chat API. wiki-server is the HTTP *client*; the model never sees MCP (stdio
`wiki-mcp` or `POST /mcp`). Tools in the chat request run in-process against
the vault, and `propose_draft` only fills the editor. **Save** is still the
only write to disk.

Leave `[llm]` out, or `provider = "none"`, and the Draft button and
sidebar Chat icon are hidden.
`/api/agent/*` 404s in that case — a guessed URL is not a working back door.

## Config

Same shape as `[embeddings]` cloud knobs: names in `config.toml`, the key
itself in an environment variable (`systemd/wiki.env.example`, typically
`/etc/opt/wiki/wiki.env` or whatever `EnvironmentFile=` the live unit
points at). Never put the key in `config.toml`.

```toml
[llm]
provider = "cloud"                          # "none" | "cloud"
api_key_env = "ANTHROPIC_API_KEY"           # NAME of an env var, not the key
api_base = "https://api.anthropic.com/v1"   # OpenAI itself: https://api.openai.com/v1
model = "claude-sonnet-4-6"
# system_prompt = """ ... """               # optional full replacement
# chat_system_prompt = """ ... """          # optional; Chat panel only
```

- `api_base` is an absolute `http(s)` URL. The client POSTs
  `{api_base}/chat/completions`. Anthropic's OpenAI-compatible layer is
  `https://api.anthropic.com/v1`; OpenAI is `https://api.openai.com/v1`.
- `api_key_env` empty is allowed (a local proxy that does not authenticate).
  If a name *is* set, that variable must be present at startup or
  `wiki-server` refuses to start — same fail-loudly rule as
  `CloudEmbeddingProvider`.
- `system_prompt` replaces the compiled Draft default when it is a
  non-empty string. Omit it, comment it out, or leave it blank to keep
  the built-in prompt (`src/llm/AgentRuntime.cpp`).
- `chat_system_prompt` is the same shape for the sidebar Chat panel
  (vault Q&A, no editor writes). Distinct from `system_prompt` — Draft
  and Chat are different jobs. Empty keeps the compiled Chat default.

Restart after changing any of this.

`CloudChatClient` lives in `wiki-server` only (httplib + OpenSSL, same
vendored header `CloudEmbeddingProvider` already uses). `AgentRuntime` is
in `libwikicore` so the tool loop can be unit-tested with a scripted
client and no HTTP.

## What the model can do

Read-only vault tools (Draft and Chat), then Draft-only write-shaped tools
that still do not write to disk:

| Tool | Where | Effect |
|---|---|---|
| `search_documents` | both | Same search as `/api/search`: FTS5, plus semantic ranking when embeddings are enabled (admin scope: public + private) |
| `get_document` | both | One document's **source** markdown (front matter stripped). A query fence is DSL text, not the live table — use `run_query_block` for that |
| `list_tags` / `list_types` / `list_documents` | both | Browse |
| `run_query_block` | both | Execute a query-block body (`key: value` lines). Same `QueryBlocks::parseAndRun` as `GET /api/query` (admin, public+private). Typos are errors, not an empty list |
| `list_document_history` | both | Past snapshots of one path, newest first (`document_snapshots`). The live file is not in the list |
| `diff_document_history` | both | Line diff of one snapshot's body versus current (same as the History page). `snapshot_id` optional — defaults to the newest. Does not restore |
| `get_current_view` | Chat | The wiki page open behind the panel (`page` / `path` / `title`). The server cannot see the browser URL; the client sends `view` on every Chat send |
| `propose_draft` | Draft | Replaces the whole editor (new notes / full rewrites) |
| `append_to_draft` | Draft | Appends markdown at the end of the current body |
| `insert_in_draft` | Draft | Inserts markdown at the editor caret (no selection) |
| `replace_in_draft` | Draft | Replaces one unique span (or the current editor selection) |

`propose_draft` is the full-document tool. Follow-ups like "add a paragraph"
or "fix the examples" should use `append_to_draft` / `insert_in_draft` /
`replace_in_draft` so the rest of the editor is left alone. If the
WYSIWYG/markdown selection is non-empty, the snapshot includes it and
`replace_in_draft` may omit `find`. The visible editor highlight goes away
when the Draft panel takes focus; the panel stashes the last non-empty
range (shown as a chip) from a pointer-down on Draft / the panel, before
that blur. Clear drops the chip and collapses the highlight in the editor
so focusing the prompt does not recapture it; selecting again in the
editor restores the chip. The caret itself also disappears on that blur
(Toast UI); the same chip shows **Insert at caret** and Clear drops it.
The insert point is read from Toast UI's own markdown/WYSIWYG model
before that blur (not from `window.getSelection()`, which jumps to the
start). `insert_in_draft` uses the stashed prefix so "here" still means
the place you clicked.

A follow-up snapshot still sends the full body for tool execution. When
there is a selection, the **model prompt** only gets an excerpt around
that span (plus the selection itself), not the whole note.

A full `propose_draft` does **not** auto-apply if the editor changed after
the turn was sent (you typed while it was Working). The panel asks
Apply anyway / Keep mine. Surgical `edit` events still apply to whatever
is currently in the editor. `setMarkdown` wipes Toast UI undo; **Revert
last draft** in the panel restores the editor (and metadata) from just
before the last applied draft.

If the instruction is ambiguous, the compiled prompt tells the model to
ask one to three short questions **in the panel** and not to call
`propose_draft` / `append_to_draft` / `insert_in_draft` /
`replace_in_draft` until you answer. A text-only reply is a normal
assistant event; the editor is left alone. Search/get are still allowed
first so the questions can be specific. A clear request (selection +
"fix this", "add a paragraph at the end", "insert here" at a known
caret, an explicit rewrite) should not stall.

Stop aborts the in-flight cloud HTTP call (`POST .../cancel`); Close still
only hides the panel. Save stays on the edit page and keeps the in-memory
session; **View** (next to Save) opens the saved document. Navigating away
drops the session.

The panel streams the model's reply: `CloudChatClient` POSTs
`stream: true` and assembles tool-call deltas the same way a blocking
response would. Incremental assistant text is one growing event in the
session (`data.streaming` while tokens are still arriving). The browser
opens `GET /api/agent/sessions/{id}/stream` (`text/event-stream`, cookie
auth, no CSRF) and paints those events as they land; a 400ms poll remains
the fallback if the stream cannot start. Tool JSON is not streamed into
the editor — `propose_draft` / the surgical edits still apply only when
the tool finishes. nginx in front of production should leave
`X-Accel-Buffering: no` alone (the handler sets it) so the proxy does not
buffer the stream.

The panel keeps the same in-memory session across Close/Draft on this edit
page, and across Save. Follow-ups send a fresh snapshot of whatever is
currently in the editor, including the selection or caret. A question-mark
control in the panel header toggles this usage summary.

Every tool call is audit-logged with a `compose:` prefix on
`mcp_audit_log`, success or failure — the Account page lists those
separately from Remote MCP activity.

Routes (admin + CSRF on mutating ones; 401/404 as appropriate):

- `POST /api/agent/sessions`
- `GET  /api/agent/sessions` (Chat history: `{sessions:[{id,title,createdAt,updatedAt}]}`)
- `GET /api/agent/sessions/{id}`
- `GET /api/agent/sessions/{id}/stream` (SSE; `?after=` skips already-seen events)
- `POST /api/agent/sessions/{id}/messages`
- `POST /api/agent/sessions/{id}/cancel`
- `POST /api/agent/sessions/{id}/title` `{title}` (Chat only)
- `DELETE /api/agent/sessions/{id}`

`GET /api/session` includes `agentEnabled` (true only when the agent is
configured *and* the caller is authenticated). That flag also reveals
the sidebar Chat icon.

## Sidebar Chat

A floating panel on every shell page, opened from a sidebar icon that
is shown only when `agentEnabled` is true (admin + `[llm]` configured).
Same `CloudChatClient`, SSE, and Stop as Draft. Read-only vault tools
only (`search_documents` / `get_document` / `get_current_view` /
`list_tags` / `list_types` / `list_documents` / `run_query_block` /
`list_document_history` / `diff_document_history`); `propose_draft` and the
surgical edit tools are not offered, and `executeTool` rejects them if
the model tries anyway.

Each chat turn includes the wiki page currently open in the browser
(`view: {page, path, title}` — document, folder, search, graph, editor,
…). The compiled prompt tells the model that "this document" / "this
folder" means that view: `get_current_view` repeats it, then
`get_document` or `list_documents` with `folder`. The server cannot see
the URL; the client sends `view` on every send. The log still shows only
the human's question, not the view JSON.

`POST /api/agent/sessions` with `{kind:"chat", instruction}` starts a
chat session (`startChat`). Follow-ups go to the same
`.../messages` route; the server picks `sendChat` from the session's
kind. Audit rows use a `chat:` prefix (Account lists them separately
from `compose:` and Remote MCP).

This app full-page-reloads on every navigation, so the panel cannot
keep a JS object alive. Chat history is SQLite `agent_chats` (not the
vault, not FTS) — the same salvage path as `mcp_audit_log`, so an
index rebuild does not wipe threads. The in-RAM map is only the
live/streaming copy; GET hydrates a persisted chat on demand. The
panel's left sidebar lists threads (New chat, rename, delete). New
chat clears the local session id without DELETE; delete is explicit
and runs `DELETE /api/agent/sessions/{id}`. `sessionStorage` keeps
the current session id plus open/geometry, and the next page
GET-restores the log and reconnects SSE with `?after=`. The list column
is resizable; that width lives in `localStorage` (`wiki.chat.sidebarWidth`),
same convenience as the site sidebar, so a hidden-panel restore cannot
clamp it to the minimum. `pagehide`
does **not** DELETE a chat session (Draft still does — it is tied to
one edit page). Title defaults to the first instruction, truncated.

Only one session may be `running` at a time (shared `ChatClient`).
Draft and Chat sessions can both exist in RAM; sending while the other
is working returns 409.

Wiki-links in answers are rendered as clickable `[[path]]` anchors in
the panel (text nodes + `<a>`, never raw innerHTML of the model text).

## Wiki-links in the editor

The compiled prompt asks for literal `[[vault/relative/path.md]]` /
`[[path.md|Label]]`. Models still over-escape punctuation in JSON tool
arguments (`\[\[path\|Label\]\]`). `propose_draft`, `append_to_draft`,
`insert_in_draft`, and `replace_in_draft` strip those escapes (several
passes — a doubled JSON escape is a real case).

Toast UI's WYSIWYG writer is CommonMark: `[[path|label]]` is not a link,
so a round-trip re-escapes the brackets (and can smash path+label into one
span). Do **not** fix that with Toast UI `widgetRules` — a live attempt
overlapped existing wiki-link documents and leaked internal `$$widgetN$$`
markers.

Instead, `static/js/common.js` maps wiki-links to a real markdown link
with an editor-only `wiki:` URL for `setMarkdown` / `initialValue`, and
maps them back to `[[ ]]` on Save / the agent snapshot:

```
[[notes/foo.md|Foo]]  ↔  [Foo](wiki:notes/foo.md)
```

Fenced code blocks are left alone so a ` ```mermaid ` (or a sample that
mentions `[[wiki-links]]`) is not rewritten. The `wiki:` scheme never
hits disk.

The default system prompt also covers Markdown `>` blockquotes and
fenced ` ```mermaid ` blocks (info string exactly `mermaid`, nothing
after).
