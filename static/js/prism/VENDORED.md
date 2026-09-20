# Vendored: Prism.js (custom bundle)

- Version: 1.30.0
- Source: individual component files from
  https://cdn.jsdelivr.net/npm/prismjs@1.30.0/components/ (jsdelivr's npm
  mirror), concatenated in dependency order into this one file:
  core, clike, c, cpp, markup, css, javascript, typescript, python, bash,
  json, yaml, toml, sql, rust, go. Order matters -- `cpp` requires `c`
  requires `clike` already defined at execution time (Prism.languages.X.extend
  reads a PRIOR language's grammar synchronously), same for
  `typescript` requiring `javascript` and `go`/`c`/`javascript` all
  requiring `clike`; see components.json in the same npm package for the
  authoritative dependency graph if this bundle is ever re-vendored with a
  different language set.
- License: MIT (Prism.js / Lea Verou & contributors)
- Checksums: see SHA256SUMS in this directory
- Deliberately NO theme CSS vendored alongside this -- `.token.*` color
  rules are hand-written directly in each css/themes/*.css file instead
  (matching the "each theme is fully self-sufficient" convention already
  established for the rest of this app, not a generic Prism theme that
  would look visually inconsistent with green/dark/classic's own palette).
- Loaded LAZILY, only on a document view that actually contains a fenced
  code block with a recognized language -- see static/js/prism-highlight.js.
- Language set chosen to cover what this project's own real content
  actually uses (see docs/architecture.md's own code fences, notes/,
  recipes/, projects/ folders) -- not an attempt at Prism's full language
  list. Add another `prism-<lang>.min.js` (respecting its own `require`
  entry in components.json) and re-concatenate if a document needs one
  that isn't here; an unrecognized language class is a silent no-op
  (Prism.highlightAllUnder leaves what it doesn't recognize alone), never
  a broken page.

Re-vendor with `tools/build-editor-bundle/fetch.sh`. Bump the version or
the language list in that script deliberately, not silently.
