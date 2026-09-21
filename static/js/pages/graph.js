// Full graph page — GET /api/graph, rendered via graph-render.js. Every
// visible document is a node, every [[wiki-link]] between two real
// documents is an edge; click a node to open that document. See
// graph-render.js's own comment for why this is a from-scratch
// force-directed layout rather than a vendored physics library.
window.WikiPages = window.WikiPages || {};

(function () {
  "use strict";

  var basePath = WikiCommon.basePath;

  var HIDE_KEY = "wiki.graph.hideUnlinked";
  var LABELS_KEY = "wiki.graph.labels";
  var LABEL_MODES = ["auto", "hover", "all"];

  function readHideUnlinked() {
    try {
      return localStorage.getItem(HIDE_KEY) === "1";
    } catch (e) {
      return false;
    }
  }

  function writeHideUnlinked(on) {
    try {
      localStorage.setItem(HIDE_KEY, on ? "1" : "0");
    } catch (e) {}
  }

  function readLabelMode() {
    try {
      var stored = localStorage.getItem(LABELS_KEY);
      if (stored && LABEL_MODES.indexOf(stored) !== -1) return stored;
    } catch (e) {}
    return "auto";
  }

  function writeLabelMode(mode) {
    try {
      localStorage.setItem(LABELS_KEY, mode);
    } catch (e) {}
  }

  // Degree-0 documents: no [[wiki-link]] edge with another real,
  // currently-existing document. "Hide unlinked" drops them (and any
  // leftover edges) so the force layout is just the actual clusters.
  function linkedSubset(data) {
    var deg = {};
    var i;
    var edges = data.edges || [];
    for (i = 0; i < edges.length; i++) {
      deg[edges[i].source] = (deg[edges[i].source] || 0) + 1;
      deg[edges[i].target] = (deg[edges[i].target] || 0) + 1;
    }
    var nodes = [];
    var keep = {};
    var src = data.nodes || [];
    for (i = 0; i < src.length; i++) {
      if ((deg[src[i].path] || 0) > 0) {
        nodes.push(src[i]);
        keep[src[i].path] = true;
      }
    }
    var keptEdges = [];
    for (i = 0; i < edges.length; i++) {
      if (keep[edges[i].source] && keep[edges[i].target]) {
        keptEdges.push(edges[i]);
      }
    }
    return { nodes: nodes, edges: keptEdges };
  }

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

    var hideUnlinked = readHideUnlinked();
    var labelMode = readLabelMode();

    container.innerHTML =
      "<h1>Graph</h1>" +
      '<div class="graph-toolbar">' +
      '<input type="search" id="graph-q" placeholder="Filter by title or content…" autocomplete="off" spellcheck="false">' +
      '<label class="checkbox-label">' +
      '<input type="checkbox" id="graph-hide-unlinked"' +
      (hideUnlinked ? " checked" : "") +
      "> Hide unlinked</label>" +
      '<label class="graph-toolbar-field">Labels ' +
      '<select id="graph-labels">' +
      '<option value="auto">Auto</option>' +
      '<option value="hover">On hover</option>' +
      '<option value="all">All</option>' +
      "</select></label>" +
      "</div>" +
      '<div id="graph-container"></div>';

    document.getElementById("graph-labels").value = labelMode;

    var lastData = null;
    var query = "";
    var matchPaths = {};
    var matchGen = 0;
    var matchTimer = null;
    var resizeTimer = null;

    function viewOpts() {
      return { query: query, labels: labelMode, matchPaths: matchPaths };
    }

    function draw(opts) {
      opts = opts || {};
      var graphContainer = document.getElementById("graph-container");
      if (!graphContainer || !lastData) return;
      if (opts.viewOnly && window.WikiGraphRender.setView) {
        if (window.WikiGraphRender.setView(viewOpts())) {
          return;
        }
      }
      var subset = hideUnlinked ? linkedSubset(lastData) : lastData;
      // Layout in the container's actual pixel size, not a hardcoded
      // 1200×800 viewBox scaled via width:100%. That old approach was
      // why the graph never used the leftover viewport: the simulation
      // ran in a fixed box and CSS letterboxed/clipped it into the
      // reading column. Measuring here (after .content--graph has
      // given the container a real height) means nodes spread across
      // whatever is actually on screen; a resize scales the cached
      // coordinates (graph-render.js) instead of simulating again.
      var rect = graphContainer.getBoundingClientRect();
      window.WikiGraphRender.render(graphContainer, subset.nodes, subset.edges, {
        width: Math.max(Math.floor(rect.width), 1),
        height: Math.max(Math.floor(rect.height), 1),
        pad: 56,
        query: query,
        labels: labelMode,
        matchPaths: matchPaths,
        emptyText: hideUnlinked ? "No linked documents." : "No documents to show.",
      });
    }

    function scheduleContentMatch() {
      if (matchTimer) clearTimeout(matchTimer);
      var gen = ++matchGen;
      if (!query) {
        matchPaths = {};
        draw({ viewOnly: true });
        return;
      }
      // Title/path substring is instant; FTS body/title/tags lands after
      // the same 300ms debounce the search page uses. A stale response
      // from an earlier keystroke is dropped via matchGen.
      draw({ viewOnly: true });
      matchTimer = setTimeout(function () {
        fetch(
          basePath() + "/api/graph/matches?q=" + encodeURIComponent(query),
          { credentials: "same-origin" }
        )
          .then(function (resp) {
            if (!resp.ok) throw new Error("HTTP " + resp.status);
            return resp.json();
          })
          .then(function (data) {
            if (gen !== matchGen) return;
            var set = {};
            var arr = data.paths || [];
            var i;
            for (i = 0; i < arr.length; i++) set[arr[i]] = true;
            matchPaths = set;
            draw({ viewOnly: true });
          })
          .catch(function () {
            if (gen !== matchGen) return;
            matchPaths = {};
            draw({ viewOnly: true });
          });
      }, 300);
    }

    document.getElementById("graph-q").addEventListener("input", function (ev) {
      query = (ev.target.value || "").trim();
      matchPaths = {};
      scheduleContentMatch();
    });

    document.getElementById("graph-hide-unlinked").addEventListener("change", function (ev) {
      hideUnlinked = !!ev.target.checked;
      writeHideUnlinked(hideUnlinked);
      draw();
    });

    document.getElementById("graph-labels").addEventListener("change", function (ev) {
      var next = ev.target.value;
      if (LABEL_MODES.indexOf(next) === -1) next = "auto";
      labelMode = next;
      writeLabelMode(labelMode);
      draw({ viewOnly: true });
    });

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
