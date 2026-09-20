// Lazy-loads Prism.js and syntax-highlights fenced code blocks on the
// document VIEW page only (deliberately -- editor-time highlighting was
// explicitly ruled out; see the Prism.js grammars' own comment in
// static/js/prism/VENDORED.md for why editing stays plain text). md4c
// already emits `<pre><code class="language-X">` for a fenced block with
// a recognized info string (see MarkdownRenderer.cpp / substituteMermaidBlocks'
// own comment on that exact shape) -- Prism.highlightAllUnder() is built
// to find exactly that selector on its own, no server-side cooperation
// needed beyond md4c already doing this for free.
//
// pre.mermaid blocks (substituteMermaidBlocks' own output) are naturally
// invisible to this: they have no nested <code class="language-..."> at
// all by the time they reach the page, so Prism's own selector never
// matches them -- no special-casing needed to keep the two features from
// colliding.
//
// Loaded lazily, same discipline as mermaid-render.js: this app's
// deployment story includes weak SBC hardware over possibly slow links,
// and the overwhelming majority of documents won't have a fenced code
// block with a recognized language at all.
window.WikiPrismHighlight = (function () {
  "use strict";

  var scriptLoadPromise = null;

  function loadScript() {
    if (window.Prism) return Promise.resolve();
    if (scriptLoadPromise) return scriptLoadPromise;
    scriptLoadPromise = new Promise(function (resolve, reject) {
      var script = document.createElement("script");
      // Relative, no basePath() prefix -- same reasoning as
      // mermaid-render.js's own script.src, see that file's comment.
      script.src = "js/prism/prism.min.js";
      script.onload = function () { resolve(); };
      script.onerror = function () {
        scriptLoadPromise = null;
        reject(new Error("failed to load prism.min.js"));
      };
      document.head.appendChild(script);
    });
    return scriptLoadPromise;
  }

  // Highlights every recognized fenced code block inside `container` in
  // place. No-op, without loading anything, if there's nothing to
  // highlight -- an unrecognized language class is left alone by Prism
  // itself (a plain, un-highlighted code block), never an error.
  function highlightIn(container) {
    var nodes = container.querySelectorAll('pre code[class*="language-"]');
    if (nodes.length === 0) return;

    loadScript()
      .then(function () {
        window.Prism.highlightAllUnder(container);
      })
      .catch(function (err) {
        console.error("Prism highlight failed:", err);
      });
  }

  return { highlightIn: highlightIn };
})();
