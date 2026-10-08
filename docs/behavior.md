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
  (`docs/embeddings.md`) and the Draft/Chat assistant
  (`docs/llm.md`), both of which need an API key you provide before
  they do anything.

## Writing content

- **No raw HTML in markdown.** `<div>`, `<script>`, any hand-typed HTML
  tag gets shown as literal text, not rendered — this is deliberate and
  is the actual security boundary against malicious documents, not a
  bug. YouTube embeds and Mermaid diagrams are special-cased separately
  and do render.
- **A `[[wiki-link]]` to a document that doesn't exist yet still
  works.** It shows as a "red" link now, and starts resolving
  automatically the moment a document is created at that path — no need
  to go back and fix anything.
- **Attachments have no size cap set by the app itself.** The real
  limit is whatever the server (2 GiB) and, if you're using one, your
  reverse proxy allow. If a large upload fails, check your reverse
  proxy's own body-size setting (`client_max_body_size` for nginx) first.
- **Every edit is saved as a version**, and any version can be restored
  — restoring itself creates a new version too, so it's never a one-way
  trip. See `/history/{path}` in the web UI.

## MCP (connecting Claude or another client)

- **Read tools are always available once MCP is set up; write tools
  are opt-in and off by default.** An MCP client can't create or modify
  documents unless you explicitly turn that on in `config.toml` or the
  Account page.
- **Every write made through MCP is logged**, whether it succeeds or
  fails, viewable from the admin panel — so you can always see what an
  AI assistant actually did to your vault.
- **The stdio MCP server doesn't rescan the vault on startup** the way
  the web server does, so if you've just started using it on a vault
  that `wiki-server` hasn't indexed yet, run `wiki-server` once (or
  `wiki-server --reindex`) first.

See `docs/mcp.md` for the full tool list and setup, `docs/embeddings.md`
and `docs/llm.md` for the optional AI features mentioned above.
