# Vendored: circuitjs1

- Upstream: [pfalstad/circuitjs1](https://github.com/pfalstad/circuitjs1)
  (`master` branch, commit `5a707168778216bb6ed01bfdd62e8bbf7ae0a032`,
  2026-09-23 — the same branch falstad.com's own production site builds
  from) — the author's own repo, not the older `sharpie7/
  circuitjs1` fork. `sharpie7`'s fork (and its own hosted copy on
  lushprojects.com) is a different, older codebase that does NOT support
  the `<cir>` XML circuit format or the `window.CircuitJS1` same-origin
  JS API — both are real, documented features
  (falstad.com/circuit/doc/js-interface.html, this repo's own README
  "Embedding" section) but only present in `pfalstad/circuitjs1` itself.
  An earlier vendor attempt used the `dev` branch's own GitHub Pages
  deployment (a different, modern ES-module build) — abandoned: that
  build's circuitjs.html could not be embedded in ANY iframe at all, on
  any site, including a plain static page with no CSP — it tried to
  frame itself and got blocked by its own code, unrelated to this app.
  `master` (classic GWT 2.8.2 output, matching what's actually live on
  falstad.com) has no such issue.
- License: GPLv2 (see `COPYING.txt` in the upstream repo) — this is the
  one vendored dependency in this project that is GPL rather than MIT;
  these static files are served as-is, unmodified build tooling, no
  wikicore/wiki-server code links against or is compiled with any of it.
- Built from source via Gradle (`./gradlew compileGwt` + `makeSite`),
  JDK 8 required (GWT 2.8.2 doesn't run on newer JDKs) — NOT scraped
  from a live deployment. See `tools/build-editor-bundle/
  fetch-circuitjs1.sh`.
- **Two deliberate source patches applied**, neither upstream stock:
  1. `UIManager.java`'s `draw()` method had its
     `if (menus.noEditCheckItem.getState()) g.drawLock(20, 30);` call
     deleted. That draws a red padlock icon over the canvas whenever the
     embed is `editable=false` — meaningless noise for a document-
     embedded, display-only schematic with no edit controls to warn
     about in the first place.
  2. `CirSim.java`'s module-load init had its final-else-branch
     `menus.getSetupList(true)` changed to `menus.getSetupList(false)`.
     With no `startCircuit`/`cct` query param set (this embed never sets
     either — see below), `openDefault=true` made the app silently fetch
     and load some default example circuit from `circuits/` the moment
     the examples list arrived, racing with and overwriting whatever
     static/js/circuit-embed.js had just imported via
     `CircuitJS1.importCircuit()`. Found live: the embedded iframe
     briefly showed the intended circuit, then snapped to an unrelated
     default LRC circuit a moment later. `false` still builds the
     (hidden, since `hideMenu=true`) "Circuits" menu, just doesn't
     auto-open anything onto the canvas.
- Each re-vendor means re-applying both patches by hand to a fresh
  checkout — `tools/build-editor-bundle/fetch-circuitjs1.sh` fails
  loudly if either patch's target text isn't found verbatim, rather than
  silently skipping it.
- **Circuit data never travels through a `cct=`/`ctz=` URL query
  parameter** — see util/MarkdownRenderer.cpp's substituteCircuitBlocks
  for why: this XML format's own attribute syntax (`attr="value"`,
  self-closing `/>`) needs literal `=` and `/` in the decoded result, and
  circuitjs1's own query-string decoder (`QueryParameters.java`, GWT's
  `URL.decode()`, which matches JS `decodeURI` not `decodeURIComponent`)
  never decodes either character back — confirmed live: a real circuit
  with `r="20"` arrived inside the iframe as the literal three
  characters `r%3D"20"` and failed to parse as XML at all, no matter how
  the value was percent-encoded beforehand. static/js/circuit-embed.js
  instead calls the documented same-origin `CircuitJS1.importCircuit()`
  JS API once the iframe's own `oncircuitjsloaded` fires, bypassing the
  URL entirely. Only `hideSidebar`/`hideMenu`/`hideInfoBox`/`editable`/
  `running` (plain ASCII, no decoder-hostile characters) stay on the
  URL.
- `frame-ancestors`/`X-Frame-Options` need a path-scoped exception in
  src/main.cpp's CSP (see that file's own comment) — this app's global
  CSP sets `frame-ancestors 'none'` on every response it serves,
  including circuitjs.html's own static files, which would otherwise
  refuse to be framed by this app's own document-view iframe.
- Checksums: see `SHA256SUMS` in this directory (covers the files as
  committed here, i.e. AFTER both patches above — not a verification
  checksum against upstream, which wouldn't match by design).

Re-vendor with `tools/build-editor-bundle/fetch-circuitjs1.sh` — it
clones the pinned commit, applies both patches, builds via Gradle, and
copies the trimmed result into this directory. Bump the pinned commit
deliberately, not silently, and re-verify both patches still apply
cleanly (upstream's own file may have moved the target lines) before
trusting a version bump.
