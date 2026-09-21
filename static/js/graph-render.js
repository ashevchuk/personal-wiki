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
    // Inset the force simulation so labels (text-anchor:middle under
    // each node, easily 80-110px wide) stay inside the viewBox instead
    // of being clipped by SVG's default overflow:hidden. Both the full
    // graph page and the local-graph rail pass this; a caller that
    // omits it (pad 0) is laying out into a box that's already padded
    // by its own CSS.
    var pad = options.pad || 0;

    container.innerHTML = "";
    if (nodes.length === 0) {
      var empty = document.createElement("p");
      empty.className = "graph-empty";
      empty.textContent = "No documents to show.";
      container.appendChild(empty);
      return;
    }

    var positions = layout(
      nodes,
      edges,
      Math.max(width - 2 * pad, 1),
      Math.max(height - 2 * pad, 1)
    );
    if (pad) {
      Object.keys(positions).forEach(function (k) {
        positions[k].x += pad;
        positions[k].y += pad;
      });
    }

    var svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("viewBox", "0 0 " + width + " " + height);
    svg.setAttribute("class", "graph-svg");

    // Everything (edges + nodes) lives inside ONE group so a single
    // transform zooms both together -- wired up by attachWheelZoom()
    // below, once this group and its own children exist.
    var viewport = document.createElementNS("http://www.w3.org/2000/svg", "g");
    viewport.setAttribute("class", "graph-viewport");
    svg.appendChild(viewport);

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
    viewport.appendChild(edgesGroup);

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
    viewport.appendChild(nodesGroup);

    container.appendChild(svg);
    attachPanZoom(svg, viewport);
  }

  // Pointer position in the SVG's own user-coordinate space (the
  // viewBox's units), NOT raw screen pixels -- getScreenCTM() is the
  // browser's own current screen<->SVG mapping, already accounting for
  // the viewBox scaling CSS does to fit the SVG's rendered box.
  // Shared by wheel-zoom (needs the cursor's svg-space point to zoom
  // toward) and drag-pan (needs the same conversion so a mouse delta
  // becomes a translate delta in the same space the transform lives in).
  function clientToSvg(svg, evt) {
    var ctm = svg.getScreenCTM();
    if (!ctm) return null;
    var pt = svg.createSVGPoint();
    pt.x = evt.clientX;
    pt.y = evt.clientY;
    return pt.matrixTransform(ctm.inverse());
  }

  // Wheel zoom toward the cursor + click-drag pan. Same transform on
  // the shared viewport group for both -- zooming in on a crowded
  // cluster is only usable if you can then slide that cluster around
  // without first zooming back out. Obsidian/maps/image-viewers all
  // pair these; zoom-only left a zoomed graph stuck off-center.
  function attachPanZoom(svg, viewport) {
    var scale = 1;
    var translateX = 0;
    var translateY = 0;
    var MIN_SCALE = 0.4;
    var MAX_SCALE = 4;
    // Multiplicative, not additive -- one wheel "click" always feels
    // like the same proportional zoom step regardless of the current
    // scale, matching how every other pan/zoom UI (maps, image viewers)
    // behaves; an additive step would feel enormous when already
    // zoomed out and imperceptible when already zoomed in.
    var ZOOM_STEP = 1.12;
    // Screen pixels, not svg-space -- a threshold in viewBox units
    // would shrink under zoom and turn a tiny twitch into a pan (and
    // swallow the click that should have opened the node). Nodes are
    // real <a href>s; a drag that never moved still has to navigate.
    // 4 was too tight: a normal click often jitters 4-6px, which
    // flipped the gesture into a pan and then the capture-phase click
    // handler below killed the <a>'s navigation (found live after pan
    // shipped, not guessed).
    var PAN_THRESHOLD_PX = 8;

    var dragging = false;
    var panning = false;
    var suppressClick = false;
    var lastSvgPt = null;
    var dragStartClientX = 0;
    var dragStartClientY = 0;

    function applyTransform() {
      viewport.setAttribute(
        "transform",
        "translate(" + translateX + "," + translateY + ") scale(" + scale + ")"
      );
    }

    function endDrag(evt) {
      if (!dragging) return;
      dragging = false;
      lastSvgPt = null;
      svg.classList.remove("is-panning");
      // pointercancel is not followed by a click -- drop the flag so
      // the NEXT real click on a node isn't swallowed. pointerup is:
      // the capture-phase click handler below consumes suppressClick
      // if this gesture actually panned.
      if (evt && evt.type === "pointercancel") suppressClick = false;
      if (evt && svg.hasPointerCapture && svg.hasPointerCapture(evt.pointerId)) {
        svg.releasePointerCapture(evt.pointerId);
      }
      panning = false;
    }

    svg.addEventListener(
      "wheel",
      function (evt) {
        // preventDefault so scrolling the wheel over the graph zooms it
        // instead of scrolling the surrounding page -- a graph embedded
        // mid-page (the local-graph widget on a document view) would
        // otherwise fight the page's own scroll on every wheel tick.
        evt.preventDefault();

        var svgPt = clientToSvg(svg, evt);
        if (!svgPt) return;

        var factor = evt.deltaY < 0 ? ZOOM_STEP : 1 / ZOOM_STEP;
        var newScale = Math.min(MAX_SCALE, Math.max(MIN_SCALE, scale * factor));
        // Re-solves translateX/Y so the SAME svg-space point under the
        // cursor lands back under the cursor after the scale changes --
        // without this, zooming would visibly drift the graph out from
        // under the mouse instead of zooming into whatever it's pointing
        // at. Derivation: screen = scale*local + translate must hold for
        // the cursor's own point both before and after, with `local`
        // (the point's position in the untransformed viewport) constant.
        var ratio = newScale / scale;
        translateX = svgPt.x - ratio * (svgPt.x - translateX);
        translateY = svgPt.y - ratio * (svgPt.y - translateY);
        scale = newScale;
        applyTransform();
      },
      { passive: false }
    );

    svg.addEventListener("pointerdown", function (evt) {
      // Left button / primary touch only -- right-click is the
      // browser's context menu, middle-click is "open in new tab" on
      // the node <a>s and should stay that.
      if (evt.button !== 0) return;
      dragging = true;
      panning = false;
      suppressClick = false;
      dragStartClientX = evt.clientX;
      dragStartClientY = evt.clientY;
      lastSvgPt = clientToSvg(svg, evt);
      // Do NOT setPointerCapture here. Capturing on the <svg> from
      // the initial pointerdown retargets the subsequent click onto
      // the svg itself, so the child <a href> never activates --
      // nodes looked clickable and did nothing. Found live after pan
      // shipped. Capture only starts once this gesture has actually
      // become a pan (see pointermove), so a still click still lands
      // on the node.
    });

    svg.addEventListener("pointermove", function (evt) {
      if (!dragging || !lastSvgPt) return;
      if (!panning) {
        var dxPx = evt.clientX - dragStartClientX;
        var dyPx = evt.clientY - dragStartClientY;
        if (dxPx * dxPx + dyPx * dyPx < PAN_THRESHOLD_PX * PAN_THRESHOLD_PX) {
          return;
        }
        panning = true;
        suppressClick = true;
        svg.classList.add("is-panning");
        // Capture only now, so a drag that leaves the svg keeps
        // panning, without the click-retarget trap above.
        if (svg.setPointerCapture) svg.setPointerCapture(evt.pointerId);
      }
      // preventDefault only once this is a real pan -- doing it on
      // every pointerdown would also suppress the click that should
      // open a node.
      evt.preventDefault();
      var svgPt = clientToSvg(svg, evt);
      if (!svgPt) return;
      // translate lives in viewBox space (same space clientToSvg
      // returns). Adding the svg-space mouse delta keeps the point
      // under the cursor glued to it: svg = scale*local + translate,
      // local constant, so d(translate) = d(svg).
      translateX += svgPt.x - lastSvgPt.x;
      translateY += svgPt.y - lastSvgPt.y;
      lastSvgPt = svgPt;
      applyTransform();
    }, { passive: false });

    svg.addEventListener("pointerup", endDrag);
    svg.addEventListener("pointercancel", endDrag);

    // Capture so we beat the node <a>'s own navigation. A drag that
    // started on a node must not follow the link; a click that never
    // crossed PAN_THRESHOLD_PX still must.
    svg.addEventListener(
      "click",
      function (evt) {
        if (!suppressClick) return;
        evt.preventDefault();
        evt.stopPropagation();
        suppressClick = false;
      },
      true
    );
  }

  return { layout: layout, neighborsOf: neighborsOf, render: render };
})();
