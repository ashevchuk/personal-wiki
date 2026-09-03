#!/usr/bin/env bash
# Re-vendors the Toast UI Editor static bundle into static/js/toastui-editor/.
# This is a build-time-only step (per docs/architecture.md — "Frontend"):
# nothing here runs on the deployed server, and Node/npm are NOT a runtime
# dependency. Re-run this only when deliberately bumping the editor version.
set -euo pipefail

VERSION="3.2.2"
DEST="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../static/js/toastui-editor" && pwd)"
BASE="https://cdn.jsdelivr.net/npm/@toast-ui/editor@${VERSION}/dist"

echo "Fetching @toast-ui/editor ${VERSION} into ${DEST} ..."
curl -sSf --max-time 30 -o "${DEST}/toastui-editor.min.js" "${BASE}/toastui-editor.min.js"
curl -sSf --max-time 30 -o "${DEST}/toastui-editor.css" "${BASE}/toastui-editor.css"

sha256sum "${DEST}/toastui-editor.min.js" "${DEST}/toastui-editor.css" \
  | sed "s|${DEST}/||" > "${DEST}/SHA256SUMS"

cat > "${DEST}/VENDORED.md" <<EOF
# Vendored: @toast-ui/editor

- Version: ${VERSION}
- Source: ${BASE}
- Fetched: $(date -u +%Y-%m-%dT%H:%M:%SZ)
- License: MIT (NHN Cloud FE Development Lab)
- Checksums: see SHA256SUMS in this directory

Re-vendor with \`tools/build-editor-bundle/fetch.sh\`. Bump \$VERSION in that
script deliberately, not silently — verify the new checksums before
committing.
EOF

echo "Done. Verify with: sha256sum -c ${DEST}/SHA256SUMS"
