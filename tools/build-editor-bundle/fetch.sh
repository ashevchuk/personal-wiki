#!/usr/bin/env bash
# Re-vendors the static/js frontend bundles (Toast UI Editor, htmx). This
# is a build-time-only step (per docs/architecture.md — "Frontend"):
# nothing here runs on the deployed server, and Node/npm are NOT a runtime
# dependency. Re-run this only when deliberately bumping a version.
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
sha256sum "${STATIC_JS}"/toastui-editor/toastui-editor.min.js "${STATIC_JS}"/toastui-editor/toastui-editor.css \
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

HTMX_VERSION="2.0.10"
vendor "htmx.org" "${HTMX_VERSION}" \
  "https://cdn.jsdelivr.net/npm/htmx.org@${HTMX_VERSION}/dist/htmx.min.js" \
  "${STATIC_JS}/htmx" "BSD 2-Clause"

echo "Done. Verify with: sha256sum -c <dest>/SHA256SUMS in each vendored dir."
