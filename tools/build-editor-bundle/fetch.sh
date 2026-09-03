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
vendor "@toast-ui/editor" "${TOASTUI_VERSION}" \
  "https://cdn.jsdelivr.net/npm/@toast-ui/editor@${TOASTUI_VERSION}/dist/toastui-editor.min.js" \
  "${STATIC_JS}/toastui-editor" "MIT (NHN Cloud FE Development Lab)"
curl -sSf --max-time 30 -o "${STATIC_JS}/toastui-editor/toastui-editor.css" \
  "https://cdn.jsdelivr.net/npm/@toast-ui/editor@${TOASTUI_VERSION}/dist/toastui-editor.css"
sha256sum "${STATIC_JS}"/toastui-editor/toastui-editor.min.js "${STATIC_JS}"/toastui-editor/toastui-editor.css \
  | sed "s|${STATIC_JS}/toastui-editor/||" > "${STATIC_JS}/toastui-editor/SHA256SUMS"

HTMX_VERSION="2.0.10"
vendor "htmx.org" "${HTMX_VERSION}" \
  "https://cdn.jsdelivr.net/npm/htmx.org@${HTMX_VERSION}/dist/htmx.min.js" \
  "${STATIC_JS}/htmx" "BSD 2-Clause"

echo "Done. Verify with: sha256sum -c <dest>/SHA256SUMS in each vendored dir."
