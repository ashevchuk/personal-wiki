# Vendored: @toast-ui/editor

- Version: 3.2.2
- Source: https://uicdn.toast.com/editor/3.2.2/toastui-editor-all.min.js
  (toastui-editor.css from the same path) — Toast's own CDN, the "-all"
  bundle. Deliberately NOT the npm package's dist/toastui-editor.js
  mirrored on jsdelivr — that one has ProseMirror as broken webpack
  externals (root[undefined]) and throws on load in a plain <script> tag;
  see the comment in tools/build-editor-bundle/fetch.sh.
- License: MIT (NHN Cloud FE Development Lab)
- Checksums: see SHA256SUMS in this directory

Re-vendor with `tools/build-editor-bundle/fetch.sh`. Bump $TOASTUI_VERSION
in that script deliberately, not silently — verify the new checksums
before committing, and sanity-check the fetched JS actually initializes
(e.g. the tools/build-editor-bundle Node vm-based check used to catch this
bug in the first place) before trusting a version bump.
