#!/usr/bin/env bash
# Re-vendors the static/js frontend bundles: Toast UI Editor, mermaid, and
# Prism.js (htmx was vendored here too until the frontend moved to a JSON
# API + client-side rendering, see docs/architecture.md, which left it
# with nothing to do). This is a build-time-only step (per
# docs/architecture.md — "Frontend"): nothing here runs on the deployed
# server, and Node/npm are NOT a runtime dependency. Re-run this only when
# deliberately bumping a version.
set -euo pipefail

STATIC_JS="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../static/js" && pwd)"

vendor() {
  local name="$1" version="$2" url="$3" dest="$4" license="$5"
  echo "Fetching ${name} ${version} into ${dest} ..."
  mkdir -p "${dest}"
  local file
  file="$(basename "${url}")"
  curl -sSf --max-time 30 -o "${dest}/${file}" "${url}"
  sha256sum "${dest}/${file}" | sed "s|${dest}/||" > "${dest}/SHA256SUMS"
  cat > "${dest}/VENDORED.md" <<EOF
# Vendored: ${name}

- Version: ${version}
- Source: ${url}
- Fetched: $(date -u +%Y-%m-%dT%H:%M:%SZ)
- License: ${license}
- Checksums: see SHA256SUMS in this directory

Re-vendor with \`tools/build-editor-bundle/fetch.sh\`. Bump the version in
that script deliberately, not silently — verify the new checksums before
committing.
EOF
}

TOASTUI_VERSION="3.2.2"
# IMPORTANT: fetch the "-all" bundle from Toast's OWN CDN (uicdn.toast.com),
# NOT "dist/toastui-editor.min.js" from the npm package mirrored on
# jsdelivr. The npm dist/toastui-editor.js has ProseMirror as webpack
# EXTERNALS with a broken browser-global mapping — its own UMD wrapper
# literally reads `root[undefined]` for all 8 prosemirror-* peers (verified
# by fetching the unminified dist/toastui-editor.js straight from jsdelivr
# and reading the UMD header: this is a real, longstanding bug baked into
# every 3.x release of the npm package itself, not a jsdelivr/minification
# artifact) — every plain <script> load throws
# "Cannot read properties of undefined (reading 'PluginKey')" the moment
# the editor's own module init runs, and no amount of pre-loading other
# global scripts can fix it, since all 8 externals collide on the same
# literal "undefined" property key. This went unnoticed from M2 until a
# real browser's console actually surfaced it — curl/HTTP-status checks on
# the served file can't catch a client-side UMD wiring bug. The "-all"
# bundle fully inlines ProseMirror instead (confirmed: no `root[undefined]`
# pattern, ~185 KB larger) and is the variant NHN's own CDN-usage docs
# actually point at for plain-script-tag usage.
curl -sSf --max-time 30 -o "${STATIC_JS}/toastui-editor/toastui-editor.min.js" \
  "https://uicdn.toast.com/editor/${TOASTUI_VERSION}/toastui-editor-all.min.js"
curl -sSf --max-time 30 -o "${STATIC_JS}/toastui-editor/toastui-editor.css" \
  "https://uicdn.toast.com/editor/${TOASTUI_VERSION}/toastui-editor.css"
# The dark theme is a separate stylesheet (scoped under a
# .toastui-editor-dark class, applied via the `theme: 'dark'` constructor
# option — see pages/edit.js) — only ever published as part of the npm
# package's dist/theme/, not on uicdn.toast.com, but it's plain CSS with
# no UMD/externals concerns, so the jsdelivr npm mirror is fine for this
# one file specifically (the root[undefined] bug above is JS-only).
curl -sSf --max-time 30 -o "${STATIC_JS}/toastui-editor/toastui-editor-dark.css" \
  "https://cdn.jsdelivr.net/npm/@toast-ui/editor@${TOASTUI_VERSION}/dist/theme/toastui-editor-dark.css"
sha256sum "${STATIC_JS}"/toastui-editor/toastui-editor.min.js \
  "${STATIC_JS}"/toastui-editor/toastui-editor.css \
  "${STATIC_JS}"/toastui-editor/toastui-editor-dark.css \
  | sed "s|${STATIC_JS}/toastui-editor/||" > "${STATIC_JS}/toastui-editor/SHA256SUMS"

# Actually evaluate the fetched bundle the way a plain <script> tag would
# (see check-toastui.js) — this is the check that would have caught the
# root[undefined] bug at vendor time instead of a live browser console.
# Soft-fails (warns, doesn't abort) only if node itself isn't available;
# any other failure aborts the vendor.
if command -v node >/dev/null 2>&1; then
  node "$(dirname "${BASH_SOURCE[0]}")/check-toastui.js" \
    "${STATIC_JS}/toastui-editor/toastui-editor.min.js"
else
  echo "WARNING: node not found — skipping the toastui-editor sanity check." >&2
  echo "  Run tools/build-editor-bundle/check-toastui.js manually before trusting this vendor." >&2
fi

cat > "${STATIC_JS}/toastui-editor/VENDORED.md" <<EOF
# Vendored: @toast-ui/editor

- Version: ${TOASTUI_VERSION}
- Source: https://uicdn.toast.com/editor/${TOASTUI_VERSION}/toastui-editor-all.min.js
  (toastui-editor.css from the same path) — Toast's own CDN, the "-all"
  bundle. Deliberately NOT the npm package's dist/toastui-editor.js
  mirrored on jsdelivr — that one has ProseMirror as broken webpack
  externals (root[undefined]) and throws on load in a plain <script> tag;
  see the comment in tools/build-editor-bundle/fetch.sh.
- License: MIT (NHN Cloud FE Development Lab)
- Checksums: see SHA256SUMS in this directory

Re-vendor with \`tools/build-editor-bundle/fetch.sh\`. Bump \$TOASTUI_VERSION
in that script deliberately, not silently — verify the new checksums
before committing, and sanity-check the fetched JS actually initializes
(e.g. the tools/build-editor-bundle Node vm-based check used to catch this
bug in the first place) before trusting a version bump.
EOF

MERMAID_VERSION="12.0.0"
# UMD build (exposes window.mermaid via a plain <script> tag), not the ESM
# build mermaid's own CDN docs demonstrate — this project has no bundler
# and no `type="module"` script tags anywhere else, so UMD is the one
# that actually fits how every other vendored bundle here gets loaded.
# ~5.3 MiB unminified-by-nature (mermaid bundles its own layout engines
# for every diagram type) — see static/js/mermaid/VENDORED.md for why
# this is loaded lazily (static/js/mermaid-render.js), never from
# shell.html directly.
mkdir -p "${STATIC_JS}/mermaid"
curl -sSf --max-time 60 -o "${STATIC_JS}/mermaid/mermaid.min.js" \
  "https://cdn.jsdelivr.net/npm/mermaid@${MERMAID_VERSION}/dist/mermaid.min.js"
sha256sum "${STATIC_JS}/mermaid/mermaid.min.js" \
  | sed "s|${STATIC_JS}/mermaid/||" > "${STATIC_JS}/mermaid/SHA256SUMS"
cat > "${STATIC_JS}/mermaid/VENDORED.md" <<EOF
# Vendored: mermaid

- Version: ${MERMAID_VERSION}
- Source: https://cdn.jsdelivr.net/npm/mermaid@${MERMAID_VERSION}/dist/mermaid.min.js
  (jsdelivr's npm mirror — mermaid's own docs only demonstrate an ESM
  build for CDN usage, but this project loads plain <script> tags
  everywhere else, so the UMD dist/mermaid.min.js build is the one that
  fits: a normal script tag load exposes window.mermaid, matching every
  other vendored bundle here (Toast UI Editor included).
- License: MIT (mermaid-js)
- Checksums: see SHA256SUMS in this directory
- Loaded LAZILY, not from shell.html: static/js/mermaid-render.js injects
  this script tag only when a rendered document actually contains a
  pre.mermaid block — this file alone is ~5.3 MiB (~1.5 MiB gzipped),
  clearly too heavy to add to every page load on a project whose whole
  deployment story includes weak SBC hardware over possibly slow links.

Re-vendor with \`tools/build-editor-bundle/fetch.sh\`. Bump \$MERMAID_VERSION
in that script deliberately, not silently — verify the new checksum
before committing.
EOF

PRISM_VERSION="1.30.0"
# Custom bundle: core + a fixed language set (see static/js/prism/VENDORED.md
# for why this set and not Prism's full list), concatenated in DEPENDENCY
# order -- cpp needs c needs clike already defined at execution time, same
# for typescript needing javascript and go/c/javascript all needing clike.
# No theme CSS vendored alongside this -- .token.* rules are hand-written
# per css/themes/*.css file instead, matching this app's existing
# each-theme-is-self-sufficient convention.
PRISM_BASE="https://cdn.jsdelivr.net/npm/prismjs@${PRISM_VERSION}/components"
PRISM_LANGS="core clike c cpp markup css javascript typescript python bash json yaml toml sql rust go"
mkdir -p "${STATIC_JS}/prism"
PRISM_TMP="$(mktemp -d)"
for lang in ${PRISM_LANGS}; do
  curl -sSf --max-time 30 -o "${PRISM_TMP}/prism-${lang}.min.js" \
    "${PRISM_BASE}/prism-${lang}.min.js"
done
: > "${STATIC_JS}/prism/prism.min.js"
for lang in ${PRISM_LANGS}; do
  cat "${PRISM_TMP}/prism-${lang}.min.js" >> "${STATIC_JS}/prism/prism.min.js"
done
rm -rf "${PRISM_TMP}"
sha256sum "${STATIC_JS}/prism/prism.min.js" \
  | sed "s|${STATIC_JS}/prism/||" > "${STATIC_JS}/prism/SHA256SUMS"
cat > "${STATIC_JS}/prism/VENDORED.md" <<EOF
# Vendored: Prism.js (custom bundle)

- Version: ${PRISM_VERSION}
- Source: individual component files from
  https://cdn.jsdelivr.net/npm/prismjs@${PRISM_VERSION}/components/
  (jsdelivr's npm mirror), concatenated in dependency order:
  ${PRISM_LANGS}
- License: MIT (Prism.js / Lea Verou & contributors)
- Checksums: see SHA256SUMS in this directory
- Deliberately NO theme CSS vendored -- .token.* color rules are
  hand-written directly in each css/themes/*.css file instead.
- Loaded LAZILY, only on a document view with a recognized fenced code
  block language -- see static/js/prism-highlight.js.

Re-vendor with \`tools/build-editor-bundle/fetch.sh\`. Bump \$PRISM_VERSION
or \$PRISM_LANGS in that script deliberately, not silently -- respect each
new language's own "require" entry in prismjs's components.json.
EOF

echo "Done. Verify with: sha256sum -c <dest>/SHA256SUMS in each vendored dir."
