# Vendored: mermaid

- Version: 12.0.0
- Source: https://cdn.jsdelivr.net/npm/mermaid@12.0.0/dist/mermaid.min.js
  (jsdelivr's npm mirror — mermaid's own docs only demonstrate an ESM
  build for CDN usage, but this project loads plain `<script>` tags
  everywhere else (no bundler, no `type="module"`), so the UMD
  `dist/mermaid.min.js` build is the one that fits: a normal script tag
  load exposes `window.mermaid`, matching every other vendored bundle
  here (Toast UI Editor included).
- License: MIT (mermaid-js)
- Checksums: see SHA256SUMS in this directory
- Loaded LAZILY, not from shell.html: static/js/mermaid-render.js injects
  this script tag only when a rendered document actually contains a
  `pre.mermaid` block — this file alone is ~5.3 MiB (~1.5 MiB gzipped),
  clearly too heavy to add to every page load on a project whose whole
  deployment story includes weak SBC hardware over possibly slow links.

Re-vendor with `tools/build-editor-bundle/fetch.sh`. Bump the version in
that script deliberately, not silently — verify the new checksum before
committing.
