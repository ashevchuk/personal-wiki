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

    container.innerHTML =
      '<h1>Graph</h1>' +
      '<p class="graph-hint">Every document as a node, every [[wiki-link]] as an edge. ' +
      "Click a node to open it.</p>" +
      '<div id="graph-container"></div>';

    fetch(basePath() + "/api/graph", { credentials: "same-origin" })
      .then(function (resp) {
        if (!resp.ok) throw new Error("HTTP " + resp.status);
        return resp.json();
      })
      .then(function (data) {
        var graphContainer = document.getElementById("graph-container");
        // A wide viewBox, not the actual viewport pixel size -- SVG's
        // own viewBox+width:100% (see the .graph-svg CSS rule) scales
        // this to whatever width the page column actually has, at any
        // screen size, without re-running the layout.
        window.WikiGraphRender.render(graphContainer, data.nodes, data.edges, {
          width: 1200,
          height: 800,
        });
      })
      .catch(function () {
        document.getElementById("graph-container").textContent = "Failed to load graph.";
      });
  };
})();
