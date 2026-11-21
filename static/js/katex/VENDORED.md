# Vendored: KaTeX

- Version: 0.19.1
- Source: https://cdn.jsdelivr.net/npm/katex@0.19.1/dist/ (katex.min.js, katex.min.css, fonts/*.woff2)
- License: MIT (Khan Academy and other contributors)
- Checksums: see SHA256SUMS in this directory
- Only the woff2 fonts are vendored; katex.min.css lists woff2 first in
  every @font-face rule, so the woff/ttf fallbacks are never requested.
- Loaded LAZILY, only on a document view containing math -- see
  static/js/math-render.js.

Re-vendor with `tools/build-editor-bundle/fetch.sh`. Bump $KATEX_VERSION
in that script deliberately, not silently -- verify the new checksums
before committing.
