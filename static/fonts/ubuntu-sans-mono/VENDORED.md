# Vendored: Ubuntu Sans Mono (Google Fonts)

- Font: Ubuntu Sans Mono, 4 faces (normal/italic × 400/700), each split
  into Google's own 6 unicode-range subsets (cyrillic-ext, cyrillic,
  greek-ext, greek, latin-ext, latin) — 24 `@font-face` rules, 12 unique
  `.woff2` files (normal and bold share identical files for every
  subset in this family — not a vendoring shortcut, that's what
  Google's own API actually serves).
- Source: `https://fonts.googleapis.com/css2?family=Ubuntu+Sans+Mono:ital,wght@0,400;0,700;1,400;1,700&display=swap`
  (`google-fonts.css` in this fetch — see `tools/build-editor-bundle/
  fetch-fonts.sh`), fetched with a modern desktop Chrome UA string so
  Google serves woff2 (its default UA otherwise serves older, larger
  formats to this project's own non-browser `curl`).
- License: SIL Open Font License 1.1 (same as every other Google Fonts
  family) — free to redistribute, including vendored/self-hosted.
- Why vendored at all: this was the ONE remaining external, live
  network dependency anywhere in this app's frontend (every other
  vendored bundle — Toast UI Editor, mermaid, Prism, circuitjs1 — was
  already self-hosted; see each one's own VENDORED.md) — a real,
  if minor, privacy leak (every green-theme page view, including a
  PRIVATE document, told fonts.gstatic.com the visitor's IP) and a real
  offline-reliability gap for a project whose whole deployment story
  targets single-board computers, not always on fast/open internet.
  `src/main.cpp`'s CSP no longer allowlists `fonts.googleapis.com`/
  `fonts.gstatic.com` at all as of this vendoring — see that file's own
  comment.
- Checksums: see `SHA256SUMS` in this directory.

Re-vendor with `tools/build-editor-bundle/fetch-fonts.sh`. Bump the
`family=` query in that script deliberately (a weight/style change,
or switching fonts entirely) — re-run it, it re-derives `fonts.css` and
the file list from whatever Google serves for the new query, no manual
editing needed.
