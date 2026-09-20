// Shared force-directed layout + SVG renderer, used by both the full
// graph page (pages/graph.js) and the per-document local graph widget
// (pages/view.js). Written from scratch rather than vendoring a physics
// library (d3-force or similar) -- a basic spring/repulsion simulation
// is a well-understood, small algorithm, and this app's real scale
// (a personal vault, tens of documents, never the thousands a generic
// PKM tool has to plan for) never needs the extra sophistication a real
// library buys (barrier collision, WebGL rendering, incremental
// re-layout). Matches the same "write it ourselves when it's small,
// vendor when it's genuinely complex" split as query-block.js/
// section-zoom.js vs. mermaid.js/Prism.js elsewhere in this app.
window.WikiGraphRender = (function () {
  "use strict";

  var basePath = WikiCommon.basePath;
  var encodeVaultPath = WikiCommon.encodeVaultPath;

  // Runs a fixed number of iterations synchronously and returns the
  // already-settled positions, rather than animating with
  // requestAnimationFrame -- cheap enough at this app's real scale that
  // the simulation finishes before the next paint anyway, so there's no
  // user-visible benefit to spreading it across frames, only added
  // complexity (start/stop/cleanup on every re-render).
  function layout(nodes, edges, width, height) {
    var n = nodes.length;
    var positions = {};
    if (n === 0) return positions;

    // Deterministic initial placement (an even circle), not
    // Math.random() -- the SAME graph settles into a recognizable, only
    // mildly-different layout across page reloads instead of a jarring
    // fresh scatter every single time.
    nodes.forEach(function (node, i) {
      var angle = (2 * Math.PI * i) / n;
      var radius = Math.min(width, height) / 3;
      positions[node.path] = {
        x: width / 2 + radius * Math.cos(angle),
        y: height / 2 + radius * Math.sin(angle),
        vx: 0,
        vy: 0,
      };
    });

    var REPULSION = 6000;
    var SPRING = 0.02;
    // 90 was too short: each node's own label is text-anchor:middle,
    // centered directly under it (see render()'s own comment on that
    // choice) -- a typical document-title label (a few words) at this
    // theme's own font-size/font-family runs 80-110px wide on its own,
    // so two directly-connected nodes at the OLD 90px rest length left
    // literally no room for either label without the two overlapping.
    // Found live on real vault content (two linked short-title
    // documents), not a synthetic worst case -- 150 gives enough margin
    // for realistic multi-word titles without visibly stretching out
    // small graphs that don't need it.
    var SPRING_LENGTH = 150;
    var DAMPING = 0.85;
    var CENTER_PULL = 0.01;
    var ITERATIONS = 250;

    for (var iter = 0; iter < ITERATIONS; iter++) {
      // Repulsion between every pair -- O(n^2), fine at real scale
      // (this app's vaults are tens of documents, not thousands).
      for (var i = 0; i < n; i++) {
        for (var j = i + 1; j < n; j++) {
          var a = positions[nodes[i].path];
          var b = positions[nodes[j].path];
          var dx = a.x - b.x;
          var dy = a.y - b.y;
          var distSq = dx * dx + dy * dy || 0.01;
          var dist = Math.sqrt(distSq);
          var force = REPULSION / distSq;
          var fx = (dx / dist) * force;
          var fy = (dy / dist) * force;
          a.vx += fx;
          a.vy += fy;
          b.vx -= fx;
          b.vy -= fy;
        }
      }

      // Spring attraction along each real edge.
      for (var e = 0; e < edges.length; e++) {
        var pa = positions[edges[e].source];
        var pb = positions[edges[e].target];
        if (!pa || !pb) continue;
        var edx = pb.x - pa.x;
        var edy = pb.y - pa.y;
        var edist = Math.sqrt(edx * edx + edy * edy) || 0.01;
        var displacement = edist - SPRING_LENGTH;
        var sforce = SPRING * displacement;
        var sfx = (edx / edist) * sforce;
        var sfy = (edy / edist) * sforce;
        pa.vx += sfx;
        pa.vy += sfy;
        pb.vx -= sfx;
        pb.vy -= sfy;
      }

      // Weak pull toward center (keeps a disconnected node from
      // drifting off into empty space) + damping + integrate.
      nodes.forEach(function (node) {
        var p = positions[node.path];
        p.vx += (width / 2 - p.x) * CENTER_PULL;
        p.vy += (height / 2 - p.y) * CENTER_PULL;
        p.vx *= DAMPING;
        p.vy *= DAMPING;
        p.x += p.vx;
        p.y += p.vy;
      });
    }

    return positions;
  }

  // Every node within `hops` edges of `centerPath`, plus the edges that
  // connect them -- the local graph is this filtered down to a small
  // neighborhood before layout(), computed CLIENT-SIDE over the same
  // full payload the full graph page uses (see GraphQueries.h's own
  // comment on why that's a legitimate simplification at this app's
  // real scale, not a scaling hack).
  function neighborsOf(centerPath, nodes, edges, hops) {
    var included = {};
    included[centerPath] = true;
    var frontier = [centerPath];
    for (var h = 0; h < hops; h++) {
      var next = [];
      for (var i = 0; i < edges.length; i++) {
        var edge = edges[i];
        if (frontier.indexOf(edge.source) !== -1 && !included[edge.target]) {
          included[edge.target] = true;
          next.push(edge.target);
        }
        if (frontier.indexOf(edge.target) !== -1 && !included[edge.source]) {
          included[edge.source] = true;
          next.push(edge.source);
        }
      }
      frontier = next;
    }
    return {
      nodes: nodes.filter(function (n) { return included[n.path]; }),
      edges: edges.filter(function (e) { return included[e.source] && included[e.target]; }),
    };
  }

  // Renders `nodes`/`edges` as an SVG force-directed graph into
  // `container`. `options.centerPath`, if given, marks that one node
  // with a distinct class (graph-node-center) for CSS to style larger/
  // differently -- used by the local graph widget to highlight "you are
  // here"; the full graph page omits it.
  function render(container, nodes, edges, options) {
    options = options || {};
    var width = options.width || 800;
    var height = options.height || 500;
    var centerPath = options.centerPath || null;

    container.innerHTML = "";
    if (nodes.length === 0) {
      var empty = document.createElement("p");
      empty.className = "graph-empty";
      empty.textContent = "No documents to show.";
      container.appendChild(empty);
      return;
    }

    var positions = layout(nodes, edges, width, height);

    var svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("viewBox", "0 0 " + width + " " + height);
    svg.setAttribute("class", "graph-svg");

    var edgesGroup = document.createElementNS("http://www.w3.org/2000/svg", "g");
    edgesGroup.setAttribute("class", "graph-edges");
    edges.forEach(function (edge) {
      var a = positions[edge.source];
      var b = positions[edge.target];
      if (!a || !b) return;
      var line = document.createElementNS("http://www.w3.org/2000/svg", "line");
      line.setAttribute("x1", a.x);
      line.setAttribute("y1", a.y);
      line.setAttribute("x2", b.x);
      line.setAttribute("y2", b.y);
      line.setAttribute("class", "graph-edge");
      edgesGroup.appendChild(line);
    });
    svg.appendChild(edgesGroup);

    var nodesGroup = document.createElementNS("http://www.w3.org/2000/svg", "g");
    nodesGroup.setAttribute("class", "graph-nodes");
    nodes.forEach(function (node) {
      var p = positions[node.path];
      if (!p) return;
      var isCenter = node.path === centerPath;

      var link = document.createElementNS("http://www.w3.org/2000/svg", "a");
      link.setAttribute("href", basePath() + "/d/" + encodeVaultPath(node.path));

      var circle = document.createElementNS("http://www.w3.org/2000/svg", "circle");
      circle.setAttribute("cx", p.x);
      circle.setAttribute("cy", p.y);
      circle.setAttribute("r", isCenter ? 10 : 6);
      circle.setAttribute("class", "graph-node" + (isCenter ? " graph-node-center" : ""));

      // .textContent, never innerHTML/string concatenation -- every node
      // built via createElementNS + real DOM properties throughout this
      // function, so a document title containing "<"/"&" is inherently
      // inert text, no manual escaping needed (unlike the HTML-string
      // rendering query-block.js does, which DOES need WikiCommon.escapeHtml).
      var label = document.createElementNS("http://www.w3.org/2000/svg", "text");
      label.setAttribute("x", p.x);
      label.setAttribute("y", p.y + (isCenter ? 22 : 18));
      label.setAttribute("class", "graph-label");
      label.textContent = node.title || node.path;

      link.appendChild(circle);
      link.appendChild(label);
      var titleEl = document.createElementNS("http://www.w3.org/2000/svg", "title");
      titleEl.textContent = node.title || node.path;
      link.appendChild(titleEl);
      nodesGroup.appendChild(link);
    });
    svg.appendChild(nodesGroup);

    container.appendChild(svg);
  }

  return { layout: layout, neighborsOf: neighborsOf, render: render };
})();
