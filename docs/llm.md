# Editor Draft agent

The edit page can show a **Draft** button that talks to an OpenAI-compatible
chat API. wiki-server is the HTTP *client*; the model never sees MCP (stdio
`wiki-mcp` or `POST /mcp`). Tools in the chat request run in-process against
the vault, and `propose_draft` only fills the editor. **Save** is still the
only write to disk.

Leave `[llm]` out, or `provider = "none"`, and the button is hidden.
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
```

- `api_base` is an absolute `http(s)` URL. The client POSTs
  `{api_base}/chat/completions`. Anthropic's OpenAI-compatible layer is
  `https://api.anthropic.com/v1`; OpenAI is `https://api.openai.com/v1`.
- `api_key_env` empty is allowed (a local proxy that does not authenticate).
  If a name *is* set, that variable must be present at startup or
  `wiki-server` refuses to start — same fail-loudly rule as
  `CloudEmbeddingProvider`.
- `system_prompt` replaces the compiled default when it is a non-empty
  string. Omit it, comment it out, or leave it blank to keep the built-in
  prompt (`src/llm/AgentRuntime.cpp`). Restart after changing any of this.

`CloudChatClient` lives in `wiki-server` only (httplib + OpenSSL, same
vendored header `CloudEmbeddingProvider` already uses). `AgentRuntime` is
in `libwikicore` so the tool loop can be unit-tested with a scripted
client and no HTTP.

## What the model can do

Read-only vault tools, then one write-shaped tool that does not write:

| Tool | Effect |
|---|---|
| `search_documents` | FTS5 search (admin scope: public + private) |
| `get_document` | One document body, capped per turn |
| `list_tags` / `list_documents` | Browse |
| `propose_draft` | Fills path/title/tags/type/body in the editor |

`propose_draft` always replaces the **whole** editor. A follow-up like
"add a paragraph at the end" is supposed to return the complete body with
that change, not a fragment. The panel keeps the same in-memory session
across Close/Draft on this edit page; Save or navigating away drops it.
Follow-ups send a fresh snapshot of whatever is currently in the editor.

Every tool call is audit-logged with a `compose:` prefix on
`mcp_audit_log`, success or failure.

Routes (admin + CSRF on mutating ones; 401/404 as appropriate):

- `POST /api/agent/sessions`
- `GET /api/agent/sessions/{id}`
- `POST /api/agent/sessions/{id}/messages`
- `DELETE /api/agent/sessions/{id}`

`GET /api/session` includes `agentEnabled` (true only when the agent is
configured *and* the caller is authenticated).

## Wiki-links in the editor

The compiled prompt asks for literal `[[vault/relative/path.md]]` /
`[[path.md|Label]]`. Models still over-escape punctuation in JSON tool
arguments (`\[\[path\|Label\]\]`). `propose_draft` strips those escapes
(several passes — a doubled JSON escape is a real case).

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
