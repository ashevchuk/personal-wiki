# Writing `query` blocks

A `query` fenced code block embeds a live, auto-updating table of documents
right inside a page — a recipe index, a project list, a "what changed
recently" digest. It re-runs on every page view, so it never goes stale the
way a hand-maintained list of links does.

## Basic syntax

Open a fenced code block with `query` as the language, then one `key: value`
pair per line:

````
```query
type: recipe
sort: title
```
````

That's the whole thing — no closing tags, no special characters to escape.
Save the document, view it, and the block becomes a real table: Title / Tags
/ Updated, each title a link to the actual document.

## Keys

| Key | What it does | Example |
|---|---|---|
| `tag` | Only documents carrying **every** listed tag (comma-separated = AND, not OR) | `tag: cpp, cheatsheet` |
| `type` | Exact match against a document's front-matter `type` | `type: recipe` |
| `folder` | Path **prefix** match — not a substring, and no wildcard needed | `folder: recipes/dinner/` |
| `orphans` | `true` — only documents nothing links to via `[[wiki-link]]` | `orphans: true` |
| `sort` | One of `title`, `updated`, `created`, `path` (default: `title`) | `sort: updated` |
| `order` | `asc` or `desc` — defaults to `desc` for `updated`/`created`, `asc` for `title`/`path` | `order: asc` |
| `limit` | 1–100 (default: 20) | `limit: 10` |
| `search` | Free text — full-text search, same engine as the main search page | `search: beet soup` |

Combine as many as you want — each line is a separate filter, all ANDed
together. Unrecognized keys, a key repeated on two lines, or a value outside
its own allowed range (e.g. `limit: 0` or `sort: nonsense`) render a plain
red error line in place of the table instead of silently showing nothing or
the wrong thing — if a block looks broken, check for a typo first.

## Full-text search with `search:`

`search:` turns a query block from a structured filter into a real
full-text search box, embedded on the page — the SAME search engine behind
the site's own search page, hybrid semantic ranking included when the
instance has embeddings configured (see `docs/embeddings.md`). A one-word
`search: systemd` behaves like typing "systemd" into the search box; a
longer phrase like `search: beet soup` benefits from semantic matching even
when the exact words "beet soup" don't appear verbatim in the target
document.

`tag`, `type`, `folder`, and `limit` all still apply as filters on top of a
`search:` query — for example, `search: beet` plus `type: recipe` plus
`folder: recipes/` narrows a full-text match down to just the recipes
folder. `sort`, `order`, and `orphans`, however, have no meaning once
results are relevance-ranked — combining any of them with `search:` is a
parse error rather than one silently overriding the other:

````
```query
search: beet soup
sort: title
```
````

renders a red error line ("search: results are always relevance-ranked --
sort/order can't be combined with search") instead of a table.

**A real example** — a full-text search box embedded right in a document:
````
```query
search: beet soup
```
````

## Real examples

**All recipes, alphabetically:**
````
```query
type: recipe
sort: title
```
````

**Just the dinner recipes:**
````
```query
folder: recipes/dinner/
```
````

**A "what's new" digest — last 5 changed documents:**
````
```query
sort: updated
limit: 5
```
````

**Documents nobody links to yet — a cleanup list:**
````
```query
orphans: true
sort: title
```
````

**Every C++ note tagged as a cheat sheet (both tags required):**
````
```query
tag: cpp, cheatsheet
```
````

**Everything in one folder, oldest first:**
````
```query
folder: projects/
sort: created
order: asc
```
````

## What it can't do (on purpose)

- **No raw SQL, no arbitrary filters beyond the table above.** This is
  deliberate, not a missing feature — see `docs/architecture.md`'s "`query`
  blocks" section for the full security reasoning (short version: a
  whitelisted DSL means a value can never become part of the query text
  itself, only a bound parameter).
- **Visibility still applies.** A private document never shows up in a query
  block's results for a visitor who isn't logged in — same fail-safe-private
  rule as everything else in this app, regardless of what the block asks for.
- **No pagination yet.** `limit` caps at 100; if you need more, narrow the
  filter instead. See `docs/architecture.md` if this ever becomes a real
  problem for a large vault — it's an easy follow-up, just not built yet.
- **The block only renders on the document VIEW page — never in the editor
  at all, currently.** While writing one, both the WYSIWYG canvas and the
  Markdown Preview panel show it as a plain, unrendered code block with a
  "query" language badge; save the document and open its view page to see
  the real table. Mermaid diagrams, by contrast, DO get a live preview in
  the editor's Markdown Preview panel (see `docs/architecture.md`'s mermaid
  section) — that's a deliberate scope choice made when that feature shipped,
  not something query blocks are structurally incapable of; the same
  customHTMLRenderer + fetch approach mermaid preview uses would work here
  too, it just hasn't been built yet.

## Draft / Chat

The same DSL runs in-process from the Draft and Chat agents
(`run_query_block` → `QueryBlocks::parseAndRun`, admin, public+private —
the agent is never anonymous). `get_document` still returns the fence
source, not the table. See `docs/llm.md`.
