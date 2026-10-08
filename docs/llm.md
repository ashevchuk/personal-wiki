# Draft and Chat agents

The edit page can show a **Draft** button that talks to an
OpenAI-compatible chat API. `wiki-server` is the HTTP *client* here — the
model itself never sees MCP (neither the stdio `wiki-mcp` nor
`POST /mcp`). Tools called during the chat request run in-process against
the vault, and `propose_draft` only fills the editor — **Save** is still
the only thing that actually writes to disk.

If `[llm]` is left out of `config.toml`, or set to `provider = "none"`,
both the Draft button and the sidebar Chat icon stay hidden, and
`/api/agent/*` returns `404` — guessing the URL isn't a working back
door.

## Config

This follows the same shape as `[embeddings]`'s cloud settings: names go
in `config.toml`, but the actual API key lives in an environment variable
(see `systemd/wiki.env.example`, typically loaded from
`/etc/opt/wiki/wiki.env` or whatever `EnvironmentFile=` the live unit
points at). Never put the raw key in `config.toml` itself.

```toml
[llm]
provider = "cloud"                          # "none" | "cloud"
api_key_env = "ANTHROPIC_API_KEY"           # NAME of an env var, not the key
api_base = "https://api.anthropic.com/v1"   # OpenAI itself: https://api.openai.com/v1
model = "claude-sonnet-4-6"
# system_prompt = """ ... """               # optional full replacement
# chat_system_prompt = """ ... """          # optional; Chat panel only
```

- `api_base` is an absolute `http(s)` URL. The client POSTs to
  `{api_base}/chat/completions`. Anthropic's OpenAI-compatible layer is
  `https://api.anthropic.com/v1`; OpenAI itself is
  `https://api.openai.com/v1`.
- `api_key_env` can be left empty, for a local proxy that doesn't
  authenticate. If a name *is* set, that environment variable must exist
  at startup, or `wiki-server` refuses to start — the same fail-loudly
  rule `CloudEmbeddingProvider` follows.
- `system_prompt`, if it's a non-empty string, replaces the compiled
  default prompt used for Draft. Leave it unset, commented out, or blank
  to keep the built-in prompt (`src/llm/AgentRuntime.cpp`).
- `chat_system_prompt` is the equivalent setting for the sidebar Chat
  panel (vault Q&A — it never writes to the editor). It's kept separate
  from `system_prompt` because Draft and Chat are genuinely different
  jobs. Leaving it empty keeps the compiled Chat default.

Restart `wiki-server` after changing any of this.

## What the model can do

Both Draft and Chat get a set of read-only vault tools. Draft additionally
gets write-*shaped* tools that still never touch disk directly — they
only edit what's currently in the browser's editor.

| Tool | Where | Effect |
|---|---|---|
| `search_documents` | both | Same search as `/api/search`: FTS5, plus semantic ranking when embeddings are enabled (admin scope: public + private) |
| `get_document` | both | One document's **source** markdown, with front matter stripped. A `query` fence returned here is DSL text, not the live rendered table — use `run_query_block` for that |
| `list_tags` / `list_types` / `list_documents` | both | Browse the vault |
| `run_query_block` | both | Executes a query-block body (`key: value` lines), using the same `QueryBlocks::parseAndRun` engine as `GET /api/query` (admin, public+private). A typo in the block is an error, never a silently empty list |
| `list_document_history` | both | Lists past snapshots of one path, newest first (`document_snapshots`) — the live file itself is not in this list |
| `diff_document_history` | both | A line diff of one snapshot's body against the current content (the same thing the History page shows). `snapshot_id` is optional and defaults to the newest. This never restores anything |
| `get_current_view` | Chat only | The wiki page open behind the panel (`page` / `path` / `title`). The server can't see the browser's URL on its own — the client sends `view` on every Chat message |
| `propose_draft` | Draft only | Replaces the whole editor — for new notes or full rewrites |
| `append_to_draft` | Draft only | Appends markdown at the end of the current body |
| `insert_in_draft` | Draft only | Inserts markdown at the editor's caret position (when there's no selection) |
| `replace_in_draft` | Draft only | Replaces one unique span of text (or the current editor selection) |

`propose_draft` is the full-document tool. A follow-up like "add a
paragraph" or "fix the examples" should use `append_to_draft`,
`insert_in_draft`, or `replace_in_draft` instead, so the rest of the
editor is left untouched.

### How Draft tracks your selection and caret

If the WYSIWYG/markdown selection is non-empty when you open Draft, the
snapshot sent to the model includes it, and `replace_in_draft` can then
omit `find` entirely (the selection itself is the target). The visible
editor highlight disappears once the Draft panel takes focus — to work
around that, the panel stashes the last non-empty selection (shown as a
small chip in the UI) from the moment you click into Draft or the panel,
just before that focus change happens. Clicking Clear drops that chip and
collapses the highlight in the editor, so simply focusing the prompt box
doesn't recapture the old selection; selecting text again in the editor
restores the chip.

The caret position has the same problem — it also disappears once Toast
UI loses focus. The same chip mechanism shows **Insert at caret** for
that case, and Clear drops it the same way. The actual insert point is
read from Toast UI's own markdown/WYSIWYG model at the moment just before
that focus change, not from `window.getSelection()` (which would jump
back to the start of the document). `insert_in_draft` uses this stashed
position so "insert here" still means the place you actually clicked.

A follow-up message always sends the full document body for tool
execution. When there's an active selection, though, the **model's own
prompt** only receives an excerpt around that span (plus the selection
itself), not the whole note — keeping the context the model reasons
over small and relevant.

### Applying a draft

A full `propose_draft` does **not** auto-apply if you edited the document
yourself while the model was working. In that case the panel asks you to
choose: Apply anyway, or Keep mine. Surgical edits (`append_to_draft`,
`insert_in_draft`, `replace_in_draft`) don't have this problem — they
always apply to whatever is currently in the editor at the moment they
run.

Applying a draft calls Toast UI's `setMarkdown`, which wipes its own undo
history. **Revert last draft**, in the panel, restores the editor (and
its metadata) back to exactly how it was just before the last applied
draft — a separate undo path for exactly this situation.

### Clarifying questions

If your instruction is ambiguous, the compiled prompt tells the model to
ask one to three short questions **in the panel**, and not to call any of
the draft-writing tools until you answer. A plain text reply from the
model is treated as a normal assistant message, and the editor is left
alone. The model can still call `search_documents`/`get_document` first,
so its questions can be specific rather than generic. A clear request —
a selection plus "fix this," "add a paragraph at the end," "insert here"
at a known caret, an explicit rewrite — shouldn't trigger this at all.

### Stopping and saving

Stop aborts the in-flight call to the cloud API (`POST .../cancel`).
Close only hides the panel — it doesn't cancel anything in flight. Save
stays on the edit page and keeps the current session in memory; **View**
(next to Save) opens the document you just saved. Navigating away from
the edit page drops the session entirely.

### Streaming

The panel streams the model's reply as it's generated, rather than
waiting for the whole response. If the stream can't start for some
reason, the UI falls back to polling every 400ms. Tool JSON itself is
never streamed into the editor — `propose_draft` and the surgical edit
tools only apply once the tool call has actually finished. If
`wiki-server` sits behind nginx in production, leave `X-Accel-Buffering: no`
alone — the app sets it itself so the proxy doesn't buffer the stream.

### Session lifetime

The panel keeps the same in-memory session across Close/reopen on one
edit page, and across Save. Each follow-up message sends a fresh snapshot
of whatever is currently in the editor, including the current selection
or caret. A small "?" control in the panel header toggles a short usage
summary.

Every tool call is audit-logged with a `compose:` prefix in
`mcp_audit_log`, whether it succeeded or failed — the Account page lists
these separately from Remote MCP activity.

### Routes

All of these require admin + CSRF on the mutating ones, with `401`/`404`
as appropriate for anything else:

- `POST /api/agent/sessions`
- `GET  /api/agent/sessions` — Chat history: `{sessions:[{id,title,createdAt,updatedAt}]}`
- `GET /api/agent/sessions/{id}`
- `GET /api/agent/sessions/{id}/stream` — SSE; `?after=` skips events already seen
- `POST /api/agent/sessions/{id}/messages`
- `POST /api/agent/sessions/{id}/cancel`
- `POST /api/agent/sessions/{id}/title` with `{title}` — Chat only
- `DELETE /api/agent/sessions/{id}`

`GET /api/session` includes an `agentEnabled` flag, true only when the
agent is both configured *and* the caller is authenticated. That same
flag controls whether the sidebar Chat icon is shown.

## Sidebar Chat

This is a floating panel available on every shell page, opened from a
sidebar icon that only appears when `agentEnabled` is true (admin +
`[llm]` configured). It uses the same `CloudChatClient`, SSE streaming,
and Stop behavior as Draft, but only gets the read-only vault tools:
`search_documents`, `get_document`, `get_current_view`, `list_tags`,
`list_types`, `list_documents`, `run_query_block`,
`list_document_history`, and `diff_document_history`. `propose_draft` and
the surgical edit tools are never offered here, and `executeTool`
actively rejects them if the model tries to call one anyway.

Every chat turn includes the wiki page currently open in the browser
(`view: {page, path, title}` — a document, folder, search, graph, or
editor page). The compiled prompt tells the model that phrases like
"this document" or "this folder" refer to that view: it calls
`get_current_view` first, then follows up with `get_document` or
`list_documents` using the right `folder`. As with Draft, the server
can't see the URL on its own; the client sends `view` on every message.
The conversation log itself still shows only your actual question, not
the view JSON that was sent alongside it.

`POST /api/agent/sessions` with `{kind:"chat", instruction}` starts a
chat session (`startChat`). Follow-up messages go to the same
`.../messages` route, and the server dispatches to `sendChat` based on
the session's own recorded kind. Audit rows for chat use a `chat:`
prefix — the Account page lists them separately from both `compose:` and
Remote MCP activity.

### Chat history and session storage

Chat history is stored in the database, not in your vault — rebuilding
the search index never wipes a chat thread. The panel's left sidebar
lists existing threads, with New chat, rename, and delete actions.
Closing the browser tab does **not** delete a chat session, so it's
still there next time; a Draft session, by contrast, is tied to the one
edit page it was opened from and does get cleared when you leave that
page. A new chat's title defaults to its first instruction, truncated.

Only one session can be `running` at a time, since they share one
`ChatClient`. Draft and Chat sessions can both exist in memory
simultaneously; sending a message while the other is still working
returns `409`.

Wiki-links that appear in a model's answer render as clickable
`[[path]]` anchors inside the panel — built from text nodes and real
`<a>` elements, never by setting the model's raw text as `innerHTML`.

## Wiki-links in the editor

The model writes normal `[[vault/relative/path.md]]` or
`[[path.md|Label]]` wiki-link syntax, and it round-trips correctly
through the WYSIWYG editor like any link you'd type yourself — this
needed some internal translation under the hood to work around the
editor's own markdown writer, but there's nothing you need to do
differently. Fenced code blocks (including ` ```mermaid ` ones) are left
untouched by this, so a code sample that happens to mention
`[[wiki-links]]` as literal text is never rewritten.

The default system prompt also covers Markdown `>` blockquotes and
fenced ` ```mermaid ` blocks, where the info string must be exactly
`mermaid` with nothing after it.
