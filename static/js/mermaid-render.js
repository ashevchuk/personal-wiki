// Lazy-loads mermaid.js and renders any ```mermaid fenced code block a
// document's rendered HTML contains — MarkdownRenderer::substituteMermaidBlocks
// (src/util/MarkdownRenderer.cpp) already turns md4c's
// `<pre><code class="language-mermaid">` output into the `<pre class="mermaid">`
// shape mermaid.js's own default selector looks for; this file's only job
// is triggering mermaid.js itself against that markup, since this app's
// document content is injected via fetch()+innerHTML (see pages/view.js),
// never present at page load for mermaid's own `startOnLoad` to catch.
//
// Loaded lazily on purpose, NOT from shell.html the way toastui-editor.min.js
// is: mermaid.min.js is ~5.3 MiB (bundles its own layout engine per diagram
// type) vs. toastui-editor's ~530 KiB — see static/js/mermaid/VENDORED.md.
// Fetching that on every single page view, for a project whose whole
// deployment story includes weak SBC hardware over possibly slow links,
// would be a real cost paid by every reader of every document, the
// overwhelming majority of which will never contain a diagram. renderIn()
// below no-ops (fetches nothing) whenever a document has no pre.mermaid
// block at all.
window.WikiMermaid = (function () {
  "use strict";

  var scriptLoadPromise = null;

  function loadScript() {
    if (window.mermaid) return Promise.resolve();
    if (scriptLoadPromise) return scriptLoadPromise;
    scriptLoadPromise = new Promise(function (resolve, reject) {
      var script = document.createElement("script");
      // Deliberately relative, no basePath() prefix -- shell.html's own
      // bootstrap <script> already sets a <base> tag every relative
      // src/href in this document resolves against, dynamically-created
      // elements included (see that script's own comment).
      script.src = "js/mermaid/mermaid.min.js";
      script.onload = function () { resolve(); };
      script.onerror = function () {
        scriptLoadPromise = null;
        reject(new Error("failed to load mermaid.min.js"));
      };
      document.head.appendChild(script);
    });
    return scriptLoadPromise;
  }

  // Same light/dark split edit.js already uses for Toast UI Editor's own
  // theme option: "classic" is the one light theme, everything else
  // (green terminal, plain dark) reads as dark for mermaid's palette too.
  function mermaidTheme() {
    var theme = document.documentElement.getAttribute("data-theme");
    return theme === "classic" ? "default" : "dark";
  }

  // Renders every pre.mermaid element inside `container` in place. No-op,
  // without loading anything, if the container has none.
  function renderIn(container) {
    var nodes = container.querySelectorAll("pre.mermaid");
    if (nodes.length === 0) return;

    loadScript()
      .then(function () {
        window.mermaid.initialize({ startOnLoad: false, theme: mermaidTheme() });
        return window.mermaid.run({ nodes: nodes });
      })
      .catch(function (err) {
        console.error("mermaid render failed:", err);
      });
  }

  return { renderIn: renderIn };
})();
