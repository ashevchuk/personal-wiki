// Full graph page — GET /api/graph, rendered via graph-render.js. Every
// visible document is a node, every [[wiki-link]] between two real
// documents is an edge; click a node to open that document. See
// graph-render.js's own comment for why this is a from-scratch
// force-directed layout rather than a vendored physics library.
window.WikiPages = window.WikiPages || {};

(function () {
  "use strict";

  var basePath = WikiCommon.basePath;

  window.WikiPages.renderGraph = function (container, session) {
    document.getElementById("page-title").textContent = "Graph — wiki";

    // .content is max-width:960px for reading documents. The graph is a
    // canvas, not a column of prose — leaving that cap in place sat the
    // SVG in a clipped 960px strip with a wide empty gutter beside it,
    // and SVG's default overflow:hidden chopped labels at the viewBox
    // edge (a real screenshot of production /graph, not guessed). This
    // class lifts the cap and stretches #graph-container over the rest
    // of the viewport (see .content--graph in each theme file). Full
    // page reload on every navigation (router.js), so the class cannot
    // leak onto a later page.
    container.classList.add("content--graph");

    container.innerHTML =
      '<h1>Graph</h1>' +
      '<p class="graph-hint">Every document as a node, every [[wiki-link]] as an edge. ' +
      "Click a node to open it. Drag to pan, scroll to zoom.</p>" +
      '<div id="graph-container"></div>';

    var lastData = null;
    var resizeTimer = null;

    function draw() {
      var graphContainer = document.getElementById("graph-container");
      if (!graphContainer || !lastData) return;
      // Layout in the container's actual pixel size, not a hardcoded
      // 1200×800 viewBox scaled via width:100%. That old approach was
      // why the graph never used the leftover viewport: the simulation
      // ran in a fixed box and CSS letterboxed/clipped it into the
      // reading column. Measuring here (after .content--graph has
      // given the container a real height) means nodes spread across
      // whatever is actually on screen; a resize re-runs it.
      var rect = graphContainer.getBoundingClientRect();
      window.WikiGraphRender.render(graphContainer, lastData.nodes, lastData.edges, {
        width: Math.max(Math.floor(rect.width), 1),
        height: Math.max(Math.floor(rect.height), 1),
        pad: 56,
      });
    }

    fetch(basePath() + "/api/graph", { credentials: "same-origin" })
      .then(function (resp) {
        if (!resp.ok) throw new Error("HTTP " + resp.status);
        return resp.json();
      })
      .then(function (data) {
        lastData = data;
        draw();
      })
      .catch(function () {
        document.getElementById("graph-container").textContent = "Failed to load graph.";
      });

    window.addEventListener("resize", function () {
      clearTimeout(resizeTimer);
      resizeTimer = setTimeout(draw, 150);
    });
  };
})();
