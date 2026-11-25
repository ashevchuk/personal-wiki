# Behavior and limits worth knowing

A handful of facts about how this app actually behaves — not how the
code is built, just what you'll notice as someone using or running it.

## Your data

- **Your markdown files on disk are the only real data.** Everything
  else (the search index, the calendar view, the graph) is derived from
  them and can be rebuilt from scratch at any time with
  `wiki-server --reindex`. If you ever need to move or back up the app,
  the vault directory is the only thing that actually matters.
- **Deleting a document doesn't really delete it.** It moves the
  document (and its attachments) into a `.trash/` folder inside the
  vault, at the same relative path. Nothing is destroyed outright.
- **Anything whose name starts with a dot is not a document.** A path
  with a dot-prefixed file or folder name (`.trash/`, `.git/`, the index
  database, ...) can't be read, written, moved or attached to through
  the web UI, the API or MCP, whoever asks. A trashed document is
  reachable only from the Trash list on the Account page.
- **Saving a document is all-or-nothing.** A save either lands
  completely or not at all — there's no state where a half-written file
  is left on disk.
- **Renaming or moving a document keeps its history, and fixes up links
  to it automatically.** Any other document that links to it gets its
  link rewritten to the new location; its version history carries over
  too.
- **If you edit vault files directly** (a text editor, `git pull`, a
  sync tool) while `wiki-server` is running, it usually notices within
  about a second and updates search automatically. This is best-effort,
  not a hard guarantee — if something ever looks out of date, restart
  the server or run `wiki-server --reindex` to force a full resync.
- **A copied document gets its own `id` automatically.** Every document
  has a unique `id` in its front matter. If you copy a file inside the
  vault, the app writes a new `id` into the copy the next time it indexes
  it; only that one line changes, and the original keeps its `id`. A
  file you rename or move outside the app keeps its `id` and its version
  history.
- **The app only rewrites the front-matter keys it manages.** Renaming
  or moving a document changes only the body of the documents that link
  to it. Saving a document from the editor keeps keys the app doesn't use
  (`aliases`, plugin settings, ...), but not YAML comments inside the
  front-matter block.

## Visibility and privacy

- **A private document is invisible, not just locked.** An
  unauthenticated request for a private document gets a plain `404`
  ("not found"), never a `403` ("forbidden") — so the fact that it
  exists at all is never revealed to someone who isn't logged in.
- **This extends to tags and links too.** A tag used only on private
  documents shows no count at all to an anonymous visitor. On the graph
  view, a link between a private document and a public one never shows
  up for an anonymous visitor either — a private document can't be
  discovered by "stepping stone" through a public one.
- **Nothing you write ever reaches an external LLM unless you turn that
  on yourself.** This app is a server that LLM clients (like Claude) can
  connect *to* for search/read/write — it doesn't call out to one on its
  own. The only features that actually send content to anyone external
  are optional and off by default: cloud-based semantic search
  ([`docs/embeddings.md`](embeddings.md)) and the Draft/Chat assistant
  ([`docs/llm.md`](llm.md)), both of which need an API key you provide
  before they do anything.

## Writing content

- **No raw HTML in markdown.** `<div>`, `<script>`, any hand-typed HTML
  tag gets shown as literal text, not rendered — this is deliberate and
  is the actual security boundary against malicious documents, not a
  bug. YouTube embeds, Mermaid diagrams and ` ```latex ` / ` ```math `
  formulas are special-cased separately and do render.
- **Math goes in ` ```latex ` or ` ```math ` blocks only.** `$...$` and
  `$$...$$` stay plain text: the WYSIWYG editor escapes `\` and `_`
  inside them on save, so they cannot survive an edit as math.
- **Attachment filenames keep their letters in any script** (Cyrillic
  included). Spaces, punctuation other than `.`, `-` and `_`, and
  invisible or bidirectional-control characters become `_`.
- **Links go to `http`, `https`, `mailto` and `tel` addresses, or to
  relative paths inside the wiki.** A link with any other scheme
  (`javascript:`, `data:`, `file:`, ...) renders as its text only, with
  nothing to click.
- **A `[[wiki-link]]` to a document that doesn't exist yet still
  works.** It shows as a "red" link now, and starts resolving
  automatically the moment a document is created at that path — no need
  to go back and fix anything.
- **An upload can be at most 100 MiB.** That is the server's limit on
  any single request, and so on an attachment uploaded through the web
  UI or remote MCP. A reverse proxy in front may allow less: if a large
  upload fails, check its body-size setting (`client_max_body_size` for
  nginx) first. A local `wiki-mcp` attaching a file by `source_path`
  copies it on disk and has no such limit.
- **Every edit is saved as a version**, and any version can be restored
  — restoring itself creates a new version too, so it's never a one-way
  trip. See `/history/{path}` in the web UI.

## MCP (connecting Claude or another client)

- **Read tools are always available once MCP is set up; write tools
  are opt-in and off by default.** An MCP client can't create or modify
  documents unless you explicitly turn that on in `config.toml` or the
  Account page.
- **Documents with `type: skill` can't be written through MCP at all**,
  even with write access on: no MCP tool (stdio or remote) and no remote
  agent call can create one, edit one, change a document's type to
  `skill`, or attach a file to one. The type is matched ignoring case
  and surrounding spaces. Skill documents are edited in the web UI only.
- **A `description` written by hand as a YAML map or list is kept as is**
  when a save leaves the Description field empty; the app cannot show or
  clear it. Replace it with text in the field, or edit the file.
- **A `type: skill` document with a `description` is offered to MCP
  clients as a skill** that Claude Code and similar clients can load into
  a session as instructions (see [`docs/mcp.md`](mcp.md)'s "Skills" for
  the naming and size rules). Anything that can write to the vault
  directory itself (an external editor, `git pull`, a sync tool) can
  therefore change those instructions; the server picks the change up
  within about a second.
- **Every write made through MCP is logged**, whether it succeeds or
  fails, viewable from the admin panel — so you can always see what an
  AI assistant actually did to your vault.
- **The stdio MCP server doesn't rescan the vault on startup** the way
  the web server does, so if you've just started using it on a vault
  that `wiki-server` hasn't indexed yet, run `wiki-server` once (or
  `wiki-server --reindex`) first.

See [`docs/mcp.md`](mcp.md) for the full tool list and setup,
[`docs/embeddings.md`](embeddings.md) and [`docs/llm.md`](llm.md) for the
optional AI features mentioned above.
