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

  function cssVar(name) {
    return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  }

  // mermaid's own built-in "dark"/"default" themes have THEIR OWN fixed
  // palette (grey node fills, white borders for "dark") -- picking one
  // per site theme (as an earlier version of this file did) still leaves
  // diagrams looking like a generic mermaid dark theme bolted onto the
  // page, not this site's own green-terminal/dark/classic palette. Using
  // `theme: "base"` + explicit `themeVariables` instead pulls the SAME
  // CSS custom properties every other themed element on the page already
  // uses, read live via getComputedStyle -- one source of truth (the
  // active css/themes/*.css file), not a second copy of these colors
  // hand-maintained here.
  //
  // Deliberately only --bg/--fg/--fg-dim/--fg-bright/--panel-bg, never
  // --border: mermaid's theming engine only recognizes hex colors (its
  // own docs are explicit about this, confirmed against the vendored
  // 12.0.0 bundle), and green.css's own --border is `rgba(0, 255, 0,
  // 0.35)`, not hex, unlike the other two themes -- --fg-dim (hex in
  // every theme) stands in for node borders/lines instead of
  // special-casing the one theme where --border wouldn't work.
  function mermaidThemeConfig() {
    return {
      theme: "base",
      themeVariables: {
        primaryColor: cssVar("--panel-bg"),
        primaryTextColor: cssVar("--fg"),
        primaryBorderColor: cssVar("--fg-dim"),
        lineColor: cssVar("--fg-dim"),
        background: cssVar("--panel-bg"),
      },
    };
  }

  // Renders every pre.mermaid element inside `container` in place. No-op,
  // without loading anything, if the container has none.
  function renderIn(container) {
    var nodes = container.querySelectorAll("pre.mermaid");
    if (nodes.length === 0) return;

    loadScript()
      .then(function () {
        var config = mermaidThemeConfig();
        config.startOnLoad = false;
        window.mermaid.initialize(config);
        return window.mermaid.run({ nodes: nodes });
      })
      .catch(function (err) {
        console.error("mermaid render failed:", err);
      });
  }

  return { renderIn: renderIn };
})();
