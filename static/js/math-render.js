// Typesets the div.math-display placeholders MarkdownRenderer emits for
// ```latex and ```math fences with KaTeX. Loaded lazily, same as
// mermaid-render.js: KaTeX's script, stylesheet and fonts are fetched only
// when a rendered document actually contains math.
window.WikiMath = (function () {
  "use strict";

  var loadPromise = null;

  // Relative URLs resolve against shell.html's <base>, see mermaid-render.js.
  function loadKatex() {
    if (window.katex) return Promise.resolve();
    if (loadPromise) return loadPromise;
    loadPromise = new Promise(function (resolve, reject) {
      var link = document.createElement("link");
      link.rel = "stylesheet";
      link.href = "js/katex/katex.min.css";
      document.head.appendChild(link);

      var script = document.createElement("script");
      script.src = "js/katex/katex.min.js";
      script.onload = function () { resolve(); };
      script.onerror = function () {
        loadPromise = null;
        reject(new Error("failed to load katex.min.js"));
      };
      document.head.appendChild(script);
    });
    return loadPromise;
  }

  function renderIn(container) {
    var nodes = container.querySelectorAll("div.math-display");
    if (nodes.length === 0) return;

    loadKatex()
      .then(function () {
        for (var i = 0; i < nodes.length; i++) {
          var el = nodes[i];
          // throwOnError: false renders an invalid formula as its source
          // in red instead of leaving the whole node untouched.
          window.katex.render(el.textContent, el, {
            displayMode: true,
            throwOnError: false,
          });
        }
      })
      .catch(function (err) {
        console.error("math render failed:", err);
      });
  }

  return { renderIn: renderIn };
})();
