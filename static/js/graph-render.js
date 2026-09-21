// Shared graph paint, used by both the full graph page (pages/graph.js)
// and the per-document local graph widget (pages/view.js).
//
// Layout lives in graph-layout.js (Barnes-Hut, O(n log n)): run in a
// Worker so a large vault does not freeze the UI thread, with a sync
// fallback on the same file if Worker construction fails. Resize does
// NOT re-run the simulation — cached coordinates are uniformly scaled
// to the new box (compounding-free: always from the original layout
// space). Matches the same "write it ourselves when it's small, vendor
// when it's genuinely complex" split as query-block.js/section-zoom.js
// vs. mermaid.js/Prism.js; Barnes-Hut is the "genuinely complex" part
// of THIS algorithm, still ours rather than d3-force.
//
// Paint is a capability ladder, not a single backend: WebGL, then
// canvas 2D, then SVG-in-the-DOM (the original renderer). Feature-
// detect a REAL getContext, not "does WebGLRenderingContext exist" --
// the constructor can be present while context creation/shader compile
// fails (blocked, software rasterizer that throws, context lost).
window.WikiGraphRender = (function () {
  "use strict";

  var basePath = WikiCommon.basePath;
  var encodeVaultPath = WikiCommon.encodeVaultPath;

  var NS = "http://www.w3.org/2000/svg";
  var CIRCLE_SEGS = 20;
  var NODE_R = 6;
  var NODE_R_MAX = 9;
  var CENTER_R = 10;
  var HIT_SLOP = 4;
  var LABEL_DY = 18;
  var CENTER_LABEL_DY = 22;
  var STROKE_W = 1.5;
  var EDGE_W = 1.5;
  var DIM_ALPHA = 0.4;
  var AUTO_LABEL_ZOOM = 1.75;

  function buildDegrees(edges) {
    var d = {};
    var i;
    for (i = 0; i < edges.length; i++) {
      d[edges[i].source] = (d[edges[i].source] || 0) + 1;
      d[edges[i].target] = (d[edges[i].target] || 0) + 1;
    }
    return d;
  }

  function matchesQuery(node, q) {
    if (!q) return true;
    var n = q.toLowerCase();
    return (
      (node.title || "").toLowerCase().indexOf(n) !== -1 ||
      (node.path || "").toLowerCase().indexOf(n) !== -1
    );
  }

  // Title/path substring (instant) OR a path from FTS content match.
  function pathMatches(path, title, view) {
    if (!view.query) return true;
    if (matchesQuery({ path: path, title: title || "" }, view.query)) return true;
    return !!(view.matchPaths && view.matchPaths[path]);
  }

  function isDimmed(node, view, hoveredPath) {
    if (!view.query) return false;
    if (node.path === hoveredPath) return false;
    if (view.centerPath && node.path === view.centerPath) return false;
    return !pathMatches(node.path, node.title, view);
  }

  function edgeIsDimmed(edge, nodes, view) {
    if (!view.query) return false;
    return (
      !pathMatches(edge.source, titleByPath(nodes, edge.source), view) &&
      !pathMatches(edge.target, titleByPath(nodes, edge.target), view)
    );
  }

  function showLabel(node, view, hoveredPath, cameraScale) {
    if (node.path === hoveredPath) return true;
    if (view.centerPath && node.path === view.centerPath) return true;
    if (view.query) return pathMatches(node.path, node.title, view);
    var mode = view.labelMode || "all";
    if (mode === "all") return true;
    if (mode === "hover") return false;
    if ((cameraScale || 1) >= AUTO_LABEL_ZOOM) return true;
    return (view.degrees[node.path] || 0) >= 2;
  }

  function makeView(edges, options) {
    options = options || {};
    var mode = options.labels || "all";
    if (mode !== "auto" && mode !== "hover" && mode !== "all") mode = "all";
    return {
      query: (options.query || "").trim(),
      labelMode: mode,
      degrees: buildDegrees(edges),
      centerPath: options.centerPath || null,
      matchPaths: options.matchPaths || {},
    };
  }

  function dimRgba(rgba, dimmed) {
    if (!dimmed) return rgba;
    return [rgba[0], rgba[1], rgba[2], rgba[3] * DIM_ALPHA];
  }

  // Keep pan/zoom across a query/label-mode redraw of the SAME node set.
  // A different node set (hide-unlinked toggle) resets to identity so
  // we don't leave the camera looking at empty space.
  var savedCamera = { scale: 1, tx: 0, ty: 0 };
  var savedCameraKey = "";

  function cameraFor(key) {
    if (savedCameraKey !== key) {
      savedCamera = { scale: 1, tx: 0, ty: 0 };
      savedCameraKey = key;
    }
    return { scale: savedCamera.scale, tx: savedCamera.tx, ty: savedCamera.ty };
  }

  function rememberCamera(camera) {
    savedCamera = { scale: camera.scale, tx: camera.tx, ty: camera.ty };
  }

  // Once WebGL has failed in this page (shader compile, context lost,
  // getContext returned null), don't keep probing it on every resize
  // redraw -- fall through to canvas/SVG for the rest of the session.
  var webglFailed = false;
  var lastBackend = null;

  // In-place view updates (query / label mode) must NOT tear down the
  // canvas: wiping it on every keystroke flashed a blank (often white)
  // WebGL buffer over the graph. live.view is the same object the
  // current backend closed over, so mutating it and repainting is
  // enough; a node-set change (hide-unlinked) still goes through
  // render() and replaces this.
  var live = null;

  var VS_SRC =
    "attribute vec2 a_pos;\n" +
    "attribute vec4 a_color;\n" +
    "uniform vec2 u_resolution;\n" +
    "uniform vec2 u_translate;\n" +
    "uniform float u_scale;\n" +
    "varying vec4 v_color;\n" +
    "void main() {\n" +
    "  vec2 world = a_pos * u_scale + u_translate;\n" +
    "  vec2 clip = (world / u_resolution) * 2.0 - 1.0;\n" +
    "  gl_Position = vec4(clip.x, -clip.y, 0.0, 1.0);\n" +
    "  v_color = a_color;\n" +
    "}\n";

  var FS_SRC =
    "precision mediump float;\n" +
    "varying vec4 v_color;\n" +
    "void main() { gl_FragColor = v_color; }\n";

  var layoutCache = { key: "", positions: null, boxW: 0, boxH: 0 };
  var layoutGen = 0;
  var layoutWorker = null;
  var workerFailed = false;
  var inflight = null;
  var pendingLayout = null;

  function graphKey(nodes, edges) {
    var np = [];
    var ep = [];
    var i;
    for (i = 0; i < nodes.length; i++) np.push(nodes[i].path);
    np.sort();
    for (i = 0; i < edges.length; i++) {
      ep.push(edges[i].source + "\t" + edges[i].target);
    }
    ep.sort();
    return np.join("\n") + "\n---\n" + ep.join("\n");
  }

  // Uniform scale + center, always from the ORIGINAL simulation box,
  // so a chain of resizes cannot compound. Non-uniform stretch would
  // squash the settled layout when the window aspect changes.
  function fitPositions(pos, fromW, fromH, toW, toH) {
    var sx = toW / (fromW || 1);
    var sy = toH / (fromH || 1);
    var s = Math.min(sx, sy);
    var ox = (toW - fromW * s) / 2;
    var oy = (toH - fromH * s) / 2;
    var out = {};
    Object.keys(pos).forEach(function (k) {
      out[k] = { x: pos[k].x * s + ox, y: pos[k].y * s + oy };
    });
    return out;
  }

  function applyPad(pos, pad) {
    if (!pad) return pos;
    var out = {};
    Object.keys(pos).forEach(function (k) {
      out[k] = { x: pos[k].x + pad, y: pos[k].y + pad };
    });
    return out;
  }

  function syncLayout(nodes, edges, width, height) {
    if (window.WikiGraphLayout && typeof window.WikiGraphLayout.layout === "function") {
      return window.WikiGraphLayout.layout(nodes, edges, width, height);
    }
    // Page script didn't load and Worker failed — circle only, no
    // physics. Better a readable ring than a thrown render.
    var positions = {};
    var n = nodes.length;
    var i;
    for (i = 0; i < n; i++) {
      var angle = (2 * Math.PI * i) / n;
      var radius = Math.min(width, height) / 3;
      positions[nodes[i].path] = {
        x: width / 2 + radius * Math.cos(angle),
        y: height / 2 + radius * Math.sin(angle),
      };
    }
    return positions;
  }

  function workerUrl() {
    var path = (basePath() || "") + "/js/graph-layout.js";
    if (path.charAt(0) !== "/") path = "/" + path;
    return path;
  }

  function finishPending(positions) {
    var p = pendingLayout;
    pendingLayout = null;
    if (p) p.cb(positions);
  }

  function bindWorker() {
    layoutWorker.onmessage = function (ev) {
      var msg = ev.data;
      if (!pendingLayout || !msg || msg.id !== pendingLayout.id) return;
      if (msg.ok && msg.positions) {
        finishPending(msg.positions);
        return;
      }
      workerFailed = true;
      var fail = pendingLayout;
      finishPending(syncLayout(fail.nodes, fail.edges, fail.boxW, fail.boxH));
    };
    layoutWorker.onerror = function () {
      workerFailed = true;
      try {
        layoutWorker.terminate();
      } catch (err) {}
      layoutWorker = null;
      if (pendingLayout) {
        var fail = pendingLayout;
        finishPending(syncLayout(fail.nodes, fail.edges, fail.boxW, fail.boxH));
      }
    };
  }

  function requestLayout(nodes, edges, boxW, boxH, id, cb) {
    if (!workerFailed && typeof Worker !== "undefined") {
      try {
        if (!layoutWorker) {
          layoutWorker = new Worker(workerUrl());
          bindWorker();
        }
        var slimNodes = [];
        var slimEdges = [];
        var i;
        for (i = 0; i < nodes.length; i++) slimNodes.push({ path: nodes[i].path });
        for (i = 0; i < edges.length; i++) {
          slimEdges.push({ source: edges[i].source, target: edges[i].target });
        }
        pendingLayout = {
          id: id,
          cb: cb,
          nodes: nodes,
          edges: edges,
          boxW: boxW,
          boxH: boxH,
        };
        layoutWorker.postMessage({
          id: id,
          nodes: slimNodes,
          edges: slimEdges,
          width: boxW,
          height: boxH,
        });
        return;
      } catch (err) {
        workerFailed = true;
        pendingLayout = null;
      }
    }
    cb(syncLayout(nodes, edges, boxW, boxH));
  }

  function nodeHref(path) {
    return basePath() + "/d/" + encodeVaultPath(path);
  }

  function nodeRadius(node, centerPath, degrees) {
    if (node.path === centerPath) return CENTER_R;
    var deg = (degrees && degrees[node.path]) || 0;
    if (deg <= 1) return NODE_R;
    return Math.min(NODE_R_MAX, NODE_R + (deg - 1) * 0.55);
  }

  function nodeLabel(node) {
    return node.title || node.path;
  }

  function readPalette(host) {
    var cs = getComputedStyle(host);
    return {
      edge: (cs.getPropertyValue("--fg-dim") || "").trim() || "#888",
      bg: (cs.getPropertyValue("--bg") || "").trim() || "#000",
      panelBg: (cs.getPropertyValue("--panel-bg") || "").trim() || "#111",
      link: (cs.getPropertyValue("--link") || "").trim() || "#4af",
      fgBright: (cs.getPropertyValue("--fg-bright") || "").trim() || "#fff",
      fgDim: (cs.getPropertyValue("--fg-dim") || "").trim() || "#999",
      fg: (cs.getPropertyValue("--fg") || "").trim() || "#ddd",
      fontFamily: cs.fontFamily || "sans-serif",
    };
  }

  // Canvas 2D can take the CSS string as-is. WebGL needs 0..1 floats.
  // Hex/rgb(a) are parsed directly; anything else (currentColor, named
  // colors, color-mix) goes through a 1×1 2D canvas so we don't have to
  // reimplement CSS color syntax.
  function cssToRgba(str) {
    var s = (str || "").trim();
    var m = s.match(/^#([0-9a-f]{6})$/i);
    if (m) {
      var h = m[1];
      return [
        parseInt(h.slice(0, 2), 16) / 255,
        parseInt(h.slice(2, 4), 16) / 255,
        parseInt(h.slice(4, 6), 16) / 255,
        1,
      ];
    }
    m = s.match(/^#([0-9a-f]{3})$/i);
    if (m) {
      var t = m[1];
      return [
        parseInt(t[0] + t[0], 16) / 255,
        parseInt(t[1] + t[1], 16) / 255,
        parseInt(t[2] + t[2], 16) / 255,
        1,
      ];
    }
    m = s.match(
      /^rgba?\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)(?:\s*,\s*([\d.]+))?\s*\)$/i
    );
    if (m) {
      return [
        Number(m[1]) / 255,
        Number(m[2]) / 255,
        Number(m[3]) / 255,
        m[4] === undefined ? 1 : Number(m[4]),
      ];
    }
    try {
      var c = document.createElement("canvas");
      c.width = c.height = 1;
      var ctx = c.getContext("2d");
      if (!ctx) return [0.5, 0.5, 0.5, 1];
      ctx.fillStyle = s;
      ctx.fillRect(0, 0, 1, 1);
      var d = ctx.getImageData(0, 0, 1, 1).data;
      return [d[0] / 255, d[1] / 255, d[2] / 255, d[3] / 255];
    } catch (err) {
      return [0.5, 0.5, 0.5, 1];
    }
  }

  function sizeCanvas(canvas, cssW, cssH) {
    var dpr = window.devicePixelRatio || 1;
    canvas.width = Math.max(1, Math.round(cssW * dpr));
    canvas.height = Math.max(1, Math.round(cssH * dpr));
    canvas.style.width = cssW + "px";
    canvas.style.height = cssH + "px";
    return dpr;
  }

  // Pointer position in the paint's own untransformed coordinate space
  // (the same units layout() and the SVG viewBox use). Not raw screen
  // pixels -- CSS may stretch the canvas/SVG to fill its box.
  function clientToView(el, evt, width, height) {
    var rect = el.getBoundingClientRect();
    if (!rect.width || !rect.height) return null;
    return {
      x: ((evt.clientX - rect.left) * width) / rect.width,
      y: ((evt.clientY - rect.top) * height) / rect.height,
    };
  }

  function viewToLayout(viewPt, camera) {
    return {
      x: (viewPt.x - camera.tx) / camera.scale,
      y: (viewPt.y - camera.ty) / camera.scale,
    };
  }

  function hitNode(nodes, positions, view, viewPt, camera) {
    if (!viewPt) return null;
    var pt = viewToLayout(viewPt, camera);
    var best = null;
    var bestDist = Infinity;
    var centerPath = view.centerPath;
    for (var i = 0; i < nodes.length; i++) {
      var node = nodes[i];
      var p = positions[node.path];
      if (!p) continue;
      var dx = p.x - pt.x;
      var dy = p.y - pt.y;
      var dist = Math.sqrt(dx * dx + dy * dy);
      var r = nodeRadius(node, centerPath, view.degrees) + HIT_SLOP;
      if (dist <= r && dist < bestDist) {
        best = node;
        bestDist = dist;
      }
    }
    return best;
  }

  function navigateTo(node, evt) {
    var href = nodeHref(node.path);
    if (evt && (evt.metaKey || evt.ctrlKey)) {
      window.open(href, "_blank");
    } else {
      window.location.href = href;
    }
  }

  function makeSrNav(nodes) {
    var nav = document.createElement("nav");
    nav.className = "graph-sr-links";
    nav.setAttribute("aria-label", "Documents in graph");
    nodes.forEach(function (node) {
      var a = document.createElement("a");
      a.setAttribute("href", nodeHref(node.path));
      a.textContent = nodeLabel(node);
      nav.appendChild(a);
    });
    return nav;
  }

  // Wheel zoom toward the cursor + click-drag pan. Same camera for
  // every backend -- SVG applies it as a group transform, canvas/WebGL
  // as a redraw. Zoom-only left a zoomed graph stuck off-center;
  // Obsidian/maps/image-viewers all pair these.
  function attachCamera(el, camera, width, height, onChange, opts) {
    opts = opts || {};
    var MIN_SCALE = 0.4;
    var MAX_SCALE = 4;
    // Multiplicative, not additive -- one wheel "click" always feels
    // like the same proportional zoom step regardless of the current
    // scale, matching how every other pan/zoom UI (maps, image viewers)
    // behaves; an additive step would feel enormous when already
    // zoomed out and imperceptible when already zoomed in.
    var ZOOM_STEP = 1.12;
    // Screen pixels, not view-space -- a threshold in viewBox units
    // would shrink under zoom and turn a tiny twitch into a pan (and
    // swallow the click that should have opened the node). 4 was too
    // tight: a normal click often jitters 4-6px, which flipped the
    // gesture into a pan (found live after pan shipped, not guessed).
    var PAN_THRESHOLD_PX = 8;

    var dragging = false;
    var panning = false;
    var suppressClick = false;
    var lastViewPt = null;
    var dragStartClientX = 0;
    var dragStartClientY = 0;
    var hoveredPath = null;

    function apply() {
      rememberCamera(camera);
      onChange(hoveredPath);
    }

    function endDrag(evt) {
      if (!dragging) return;
      dragging = false;
      lastViewPt = null;
      el.classList.remove("is-panning");
      if (el.parentNode) el.parentNode.classList.remove("is-panning");
      // pointercancel is not followed by a click -- drop the flag so
      // the NEXT real click on a node isn't swallowed. pointerup is:
      // the click handler below consumes suppressClick if this gesture
      // actually panned.
      if (evt && evt.type === "pointercancel") suppressClick = false;
      if (evt && el.hasPointerCapture && el.hasPointerCapture(evt.pointerId)) {
        el.releasePointerCapture(evt.pointerId);
      }
      panning = false;
    }

    el.addEventListener(
      "wheel",
      function (evt) {
        // preventDefault so scrolling the wheel over the graph zooms it
        // instead of scrolling the surrounding page -- a graph embedded
        // mid-page (the local-graph widget on a document view) would
        // otherwise fight the page's own scroll on every wheel tick.
        evt.preventDefault();
        var viewPt = clientToView(el, evt, width, height);
        if (!viewPt) return;
        var factor = evt.deltaY < 0 ? ZOOM_STEP : 1 / ZOOM_STEP;
        var newScale = Math.min(MAX_SCALE, Math.max(MIN_SCALE, camera.scale * factor));
        // Re-solves translate so the SAME view-space point under the
        // cursor lands back under the cursor after the scale changes --
        // without this, zooming would visibly drift the graph out from
        // under the mouse. Derivation: view = scale*local + translate
        // must hold for the cursor's own point both before and after.
        var ratio = newScale / camera.scale;
        camera.tx = viewPt.x - ratio * (viewPt.x - camera.tx);
        camera.ty = viewPt.y - ratio * (viewPt.y - camera.ty);
        camera.scale = newScale;
        apply();
      },
      { passive: false }
    );

    el.addEventListener("pointerdown", function (evt) {
      // Left button / primary touch only -- right-click is the
      // browser's context menu, middle-click is "open in new tab".
      if (evt.button !== 0) return;
      dragging = true;
      panning = false;
      suppressClick = false;
      dragStartClientX = evt.clientX;
      dragStartClientY = evt.clientY;
      lastViewPt = clientToView(el, evt, width, height);
      // Do NOT setPointerCapture here. Capturing on the element from
      // the initial pointerdown retargets the subsequent click onto
      // it, so a child <a href> (SVG) never activates -- nodes looked
      // clickable and did nothing. Found live after pan shipped.
      // Capture only starts once this gesture has actually become a
      // pan (see pointermove).
    });

    el.addEventListener(
      "pointermove",
      function (evt) {
        if (!dragging || !lastViewPt) {
          if (!dragging && opts.hitTest && !panning) {
            var hoverPt = clientToView(el, evt, width, height);
            var hoverNode = opts.hitTest(hoverPt);
            var nextPath = hoverNode ? hoverNode.path : null;
            el.style.cursor = nextPath ? "pointer" : "";
            if (nextPath !== hoveredPath) {
              hoveredPath = nextPath;
              apply();
            }
          }
          return;
        }
        if (!panning) {
          var dxPx = evt.clientX - dragStartClientX;
          var dyPx = evt.clientY - dragStartClientY;
          if (dxPx * dxPx + dyPx * dyPx < PAN_THRESHOLD_PX * PAN_THRESHOLD_PX) {
            return;
          }
          panning = true;
          suppressClick = true;
          el.classList.add("is-panning");
          if (el.parentNode) el.parentNode.classList.add("is-panning");
          el.style.cursor = "";
          if (el.setPointerCapture) el.setPointerCapture(evt.pointerId);
        }
        // preventDefault only once this is a real pan -- doing it on
        // every pointerdown would also suppress the click that should
        // open a node.
        evt.preventDefault();
        var viewPt = clientToView(el, evt, width, height);
        if (!viewPt) return;
        camera.tx += viewPt.x - lastViewPt.x;
        camera.ty += viewPt.y - lastViewPt.y;
        lastViewPt = viewPt;
        apply();
      },
      { passive: false }
    );

    el.addEventListener("pointerup", endDrag);
    el.addEventListener("pointercancel", endDrag);
    el.addEventListener("pointerleave", function () {
      if (dragging) return;
      if (hoveredPath) {
        hoveredPath = null;
        el.style.cursor = "";
        apply();
      }
    });

    el.addEventListener(
      "click",
      function (evt) {
        if (suppressClick) {
          evt.preventDefault();
          evt.stopPropagation();
          suppressClick = false;
          return;
        }
        if (!opts.onActivate) return;
        var node = opts.hitTest && opts.hitTest(clientToView(el, evt, width, height));
        if (!node) return;
        evt.preventDefault();
        opts.onActivate(node, evt);
      },
      true
    );

    if (opts.onActivate) {
      el.addEventListener("auxclick", function (evt) {
        if (evt.button !== 1) return;
        var node = opts.hitTest && opts.hitTest(clientToView(el, evt, width, height));
        if (!node) return;
        evt.preventDefault();
        window.open(nodeHref(node.path), "_blank");
      });
    }
  }

  function compileShader(gl, type, src) {
    var sh = gl.createShader(type);
    gl.shaderSource(sh, src);
    gl.compileShader(sh);
    if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
      gl.deleteShader(sh);
      return null;
    }
    return sh;
  }

  function compileProgram(gl, vsSrc, fsSrc) {
    var vs = compileShader(gl, gl.VERTEX_SHADER, vsSrc);
    var fs = compileShader(gl, gl.FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs) {
      if (vs) gl.deleteShader(vs);
      if (fs) gl.deleteShader(fs);
      return null;
    }
    var prog = gl.createProgram();
    gl.attachShader(prog, vs);
    gl.attachShader(prog, fs);
    gl.linkProgram(prog);
    gl.deleteShader(vs);
    gl.deleteShader(fs);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
      gl.deleteProgram(prog);
      return null;
    }
    return prog;
  }

  function pushVert(arr, x, y, rgba) {
    arr.push(x, y, rgba[0], rgba[1], rgba[2], rgba[3]);
  }

  function appendLineQuad(arr, x1, y1, x2, y2, width, rgba) {
    var dx = x2 - x1;
    var dy = y2 - y1;
    var len = Math.sqrt(dx * dx + dy * dy) || 0.01;
    var nx = (-dy / len) * (width / 2);
    var ny = (dx / len) * (width / 2);
    pushVert(arr, x1 + nx, y1 + ny, rgba);
    pushVert(arr, x1 - nx, y1 - ny, rgba);
    pushVert(arr, x2 + nx, y2 + ny, rgba);
    pushVert(arr, x1 - nx, y1 - ny, rgba);
    pushVert(arr, x2 - nx, y2 - ny, rgba);
    pushVert(arr, x2 + nx, y2 + ny, rgba);
  }

  function appendDisk(arr, cx, cy, r, rgba) {
    var i;
    for (i = 0; i < CIRCLE_SEGS; i++) {
      var a0 = (2 * Math.PI * i) / CIRCLE_SEGS;
      var a1 = (2 * Math.PI * (i + 1)) / CIRCLE_SEGS;
      pushVert(arr, cx, cy, rgba);
      pushVert(arr, cx + r * Math.cos(a0), cy + r * Math.sin(a0), rgba);
      pushVert(arr, cx + r * Math.cos(a1), cy + r * Math.sin(a1), rgba);
    }
  }

  function appendRing(arr, cx, cy, inner, outer, rgba) {
    var i;
    for (i = 0; i < CIRCLE_SEGS; i++) {
      var a0 = (2 * Math.PI * i) / CIRCLE_SEGS;
      var a1 = (2 * Math.PI * (i + 1)) / CIRCLE_SEGS;
      var x0i = cx + inner * Math.cos(a0);
      var y0i = cy + inner * Math.sin(a0);
      var x0o = cx + outer * Math.cos(a0);
      var y0o = cy + outer * Math.sin(a0);
      var x1i = cx + inner * Math.cos(a1);
      var y1i = cy + inner * Math.sin(a1);
      var x1o = cx + outer * Math.cos(a1);
      var y1o = cy + outer * Math.sin(a1);
      pushVert(arr, x0i, y0i, rgba);
      pushVert(arr, x0o, y0o, rgba);
      pushVert(arr, x1o, y1o, rgba);
      pushVert(arr, x0i, y0i, rgba);
      pushVert(arr, x1o, y1o, rgba);
      pushVert(arr, x1i, y1i, rgba);
    }
  }

  function nodeFillStroke(node, view, hoveredPath, palette) {
    var isCenter = node.path === view.centerPath;
    var hovered = node.path === hoveredPath;
    var dimmed = isDimmed(node, view, hoveredPath);
    if (isCenter) {
      return { fill: palette.link, stroke: palette.fgBright, dimmed: false };
    }
    return {
      fill: hovered ? palette.link : palette.panelBg,
      stroke: dimmed ? palette.fgDim : palette.link,
      dimmed: dimmed,
    };
  }

  function drawLabels2d(ctx, dpr, camera, nodes, positions, view, hoveredPath, palette) {
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.clearRect(0, 0, ctx.canvas.width, ctx.canvas.height);
    ctx.setTransform(
      dpr * camera.scale,
      0,
      0,
      dpr * camera.scale,
      dpr * camera.tx,
      dpr * camera.ty
    );
    ctx.font = "11px " + palette.fontFamily;
    ctx.textAlign = "center";
    ctx.textBaseline = "alphabetic";
    nodes.forEach(function (node) {
      if (!showLabel(node, view, hoveredPath, camera.scale)) return;
      var p = positions[node.path];
      if (!p) return;
      ctx.globalAlpha = isDimmed(node, view, hoveredPath) ? DIM_ALPHA : 1;
      ctx.fillStyle = node.path === hoveredPath ? palette.fg : palette.fgDim;
      ctx.fillText(
        nodeLabel(node),
        p.x,
        p.y + (node.path === view.centerPath ? CENTER_LABEL_DY : LABEL_DY)
      );
    });
    ctx.globalAlpha = 1;
  }

  function drawGraph2d(ctx, dpr, camera, nodes, edges, positions, view, hoveredPath, palette) {
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.clearRect(0, 0, ctx.canvas.width, ctx.canvas.height);
    ctx.fillStyle = palette.bg;
    ctx.fillRect(0, 0, ctx.canvas.width, ctx.canvas.height);
    ctx.setTransform(
      dpr * camera.scale,
      0,
      0,
      dpr * camera.scale,
      dpr * camera.tx,
      dpr * camera.ty
    );
    ctx.lineCap = "round";
    ctx.strokeStyle = palette.edge;
    ctx.lineWidth = EDGE_W;
    edges.forEach(function (edge) {
      var a = positions[edge.source];
      var b = positions[edge.target];
      if (!a || !b) return;
      var edgeDim = edgeIsDimmed(edge, nodes, view);
      ctx.globalAlpha = edgeDim ? DIM_ALPHA : 1;
      ctx.beginPath();
      ctx.moveTo(a.x, a.y);
      ctx.lineTo(b.x, b.y);
      ctx.stroke();
    });
    nodes.forEach(function (node) {
      var p = positions[node.path];
      if (!p) return;
      var r = nodeRadius(node, view.centerPath, view.degrees);
      var colors = nodeFillStroke(node, view, hoveredPath, palette);
      ctx.globalAlpha = colors.dimmed ? DIM_ALPHA : 1;
      ctx.beginPath();
      ctx.arc(p.x, p.y, r, 0, Math.PI * 2);
      ctx.fillStyle = colors.fill;
      ctx.fill();
      ctx.lineWidth = STROKE_W;
      ctx.strokeStyle = colors.stroke;
      ctx.stroke();
      if (!showLabel(node, view, hoveredPath, camera.scale)) return;
      ctx.font = "11px " + palette.fontFamily;
      ctx.textAlign = "center";
      ctx.textBaseline = "alphabetic";
      ctx.fillStyle = node.path === hoveredPath ? palette.fg : palette.fgDim;
      ctx.fillText(
        nodeLabel(node),
        p.x,
        p.y + (node.path === view.centerPath ? CENTER_LABEL_DY : LABEL_DY)
      );
    });
    ctx.globalAlpha = 1;
  }

  function titleByPath(nodes, path) {
    var i;
    for (i = 0; i < nodes.length; i++) {
      if (nodes[i].path === path) return nodes[i].title || "";
    }
    return "";
  }

  function buildWebGLGeometry(nodes, edges, positions, view, hoveredPath, palette) {
    var floats = [];
    var edgeRgba = cssToRgba(palette.edge);
    edges.forEach(function (edge) {
      var a = positions[edge.source];
      var b = positions[edge.target];
      if (!a || !b) return;
      var edgeDim = edgeIsDimmed(edge, nodes, view);
      // Quads, not GL_LINES — lineWidth is ignored on most WebGL
      // implementations, so a 1px hairline would stay dark-looking even
      // after the color bump. Same 1.5 CSS-px thickness as SVG/canvas.
      appendLineQuad(floats, a.x, a.y, b.x, b.y, EDGE_W, dimRgba(edgeRgba, edgeDim));
    });
    nodes.forEach(function (node) {
      var p = positions[node.path];
      if (!p) return;
      var r = nodeRadius(node, view.centerPath, view.degrees);
      var colors = nodeFillStroke(node, view, hoveredPath, palette);
      var inner = Math.max(r - STROKE_W / 2, 0.5);
      var outer = r + STROKE_W / 2;
      appendDisk(floats, p.x, p.y, inner, dimRgba(cssToRgba(colors.fill), colors.dimmed));
      appendRing(floats, p.x, p.y, inner, outer, dimRgba(cssToRgba(colors.stroke), colors.dimmed));
    });
    return { data: new Float32Array(floats), totalVerts: floats.length / 6 };
  }

  function tryWebGL(container, nodes, edges, positions, width, height, palette, view, graphKey) {
    if (webglFailed) return false;

    var surface = document.createElement("div");
    surface.className = "graph-surface";
    var canvas = document.createElement("canvas");
    canvas.className = "graph-canvas";
    canvas.setAttribute("role", "img");
    canvas.setAttribute("aria-label", "Document graph");
    sizeCanvas(canvas, width, height);

    var gl = null;
    try {
      var attrs = { antialias: true, alpha: true, premultipliedAlpha: false };
      gl = canvas.getContext("webgl", attrs) || canvas.getContext("experimental-webgl", attrs);
    } catch (err) {
      gl = null;
    }
    if (!gl) {
      webglFailed = true;
      return false;
    }

    var program = compileProgram(gl, VS_SRC, FS_SRC);
    if (!program) {
      webglFailed = true;
      var lose = gl.getExtension("WEBGL_lose_context");
      if (lose) lose.loseContext();
      return false;
    }

    var locPos = gl.getAttribLocation(program, "a_pos");
    var locColor = gl.getAttribLocation(program, "a_color");
    var uRes = gl.getUniformLocation(program, "u_resolution");
    var uTrans = gl.getUniformLocation(program, "u_translate");
    var uScale = gl.getUniformLocation(program, "u_scale");
    var buffer = gl.createBuffer();
    var STRIDE = 24;
    var geom = null;
    var geomDirty = true;
    var bgRgba = cssToRgba(palette.bg);

    function upload(hoveredPath) {
      geom = buildWebGLGeometry(nodes, edges, positions, view, hoveredPath, palette);
      gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
      gl.bufferData(gl.ARRAY_BUFFER, geom.data, gl.DYNAMIC_DRAW);
    }

    var labels = document.createElement("canvas");
    labels.className = "graph-canvas graph-canvas-labels";
    labels.setAttribute("aria-hidden", "true");
    var labelDpr = sizeCanvas(labels, width, height);
    var labelCtx = labels.getContext("2d");
    if (!labelCtx) {
      webglFailed = true;
      return false;
    }

    var camera = cameraFor(graphKey);
    var lastHover = null;
    upload(null);
    geomDirty = false;

    function paint(hoveredPath) {
      if (gl.isContextLost()) return;
      if (geomDirty || hoveredPath !== lastHover) {
        lastHover = hoveredPath;
        geomDirty = false;
        upload(hoveredPath);
      }
      gl.viewport(0, 0, canvas.width, canvas.height);
      // Opaque clear so a screenshot/composite of the GL buffer is the
      // page background, not a transparent hole that some capture paths
      // paint as white. Labels sit on a separate 2D canvas above this.
      gl.clearColor(bgRgba[0], bgRgba[1], bgRgba[2], 1);
      gl.clear(gl.COLOR_BUFFER_BIT);
      gl.enable(gl.BLEND);
      gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
      gl.useProgram(program);
      gl.uniform2f(uRes, width, height);
      gl.uniform2f(uTrans, camera.tx, camera.ty);
      gl.uniform1f(uScale, camera.scale);
      gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
      gl.enableVertexAttribArray(locPos);
      gl.vertexAttribPointer(locPos, 2, gl.FLOAT, false, STRIDE, 0);
      gl.enableVertexAttribArray(locColor);
      gl.vertexAttribPointer(locColor, 4, gl.FLOAT, false, STRIDE, 8);
      if (geom.totalVerts) {
        gl.drawArrays(gl.TRIANGLES, 0, geom.totalVerts);
      }
      drawLabels2d(
        labelCtx,
        labelDpr,
        camera,
        nodes,
        positions,
        view,
        hoveredPath,
        palette
      );
    }

    canvas.addEventListener("webglcontextlost", function (evt) {
      evt.preventDefault();
      webglFailed = true;
    });

    surface.appendChild(canvas);
    surface.appendChild(labels);
    surface.appendChild(makeSrNav(nodes));
    container.appendChild(surface);

    attachCamera(canvas, camera, width, height, paint, {
      hitTest: function (viewPt) {
        return hitNode(nodes, positions, view, viewPt, camera);
      },
      onActivate: navigateTo,
    });
    paint(null);
    live = {
      view: view,
      repaint: function () {
        geomDirty = true;
        paint(lastHover);
      },
    };
    lastBackend = "webgl";
    return true;
  }

  function tryCanvas(container, nodes, edges, positions, width, height, palette, view, graphKey) {
    var canvas = document.createElement("canvas");
    var ctx = null;
    try {
      ctx = canvas.getContext("2d");
    } catch (err) {
      ctx = null;
    }
    if (!ctx) return false;

    var surface = document.createElement("div");
    surface.className = "graph-surface";
    canvas.className = "graph-canvas";
    canvas.setAttribute("role", "img");
    canvas.setAttribute("aria-label", "Document graph");
    var dpr = sizeCanvas(canvas, width, height);
    var camera = cameraFor(graphKey);

    function paint(hoveredPath) {
      drawGraph2d(ctx, dpr, camera, nodes, edges, positions, view, hoveredPath, palette);
    }

    surface.appendChild(canvas);
    surface.appendChild(makeSrNav(nodes));
    container.appendChild(surface);

    attachCamera(canvas, camera, width, height, paint, {
      hitTest: function (viewPt) {
        return hitNode(nodes, positions, view, viewPt, camera);
      },
      onActivate: navigateTo,
    });
    paint(null);
    live = {
      view: view,
      repaint: function () {
        paint(null);
      },
    };
    lastBackend = "canvas";
    return true;
  }

  function renderSvg(container, nodes, edges, positions, width, height, view, graphKey) {
    var svg = document.createElementNS(NS, "svg");
    svg.setAttribute("viewBox", "0 0 " + width + " " + height);
    svg.setAttribute("class", "graph-svg");

    // Everything (edges + nodes) lives inside ONE group so a single
    // transform zooms both together -- wired up by attachCamera below.
    var viewport = document.createElementNS(NS, "g");
    viewport.setAttribute("class", "graph-viewport");
    svg.appendChild(viewport);

    var edgesGroup = document.createElementNS(NS, "g");
    edgesGroup.setAttribute("class", "graph-edges");
    var edgeEls = [];
    edges.forEach(function (edge) {
      var a = positions[edge.source];
      var b = positions[edge.target];
      if (!a || !b) return;
      var line = document.createElementNS(NS, "line");
      line.setAttribute("x1", a.x);
      line.setAttribute("y1", a.y);
      line.setAttribute("x2", b.x);
      line.setAttribute("y2", b.y);
      var edgeDim = edgeIsDimmed(edge, nodes, view);
      line.setAttribute("class", "graph-edge" + (edgeDim ? " graph-dim" : ""));
      edgesGroup.appendChild(line);
      edgeEls.push({ edge: edge, el: line });
    });
    viewport.appendChild(edgesGroup);

    var nodesGroup = document.createElementNS(NS, "g");
    nodesGroup.setAttribute("class", "graph-nodes");
    var labelEls = [];
    nodes.forEach(function (node) {
      var p = positions[node.path];
      if (!p) return;
      var isCenter = node.path === view.centerPath;

      var link = document.createElementNS(NS, "a");
      link.setAttribute("href", nodeHref(node.path));
      if (isDimmed(node, view, null)) link.setAttribute("class", "graph-dim");

      var circle = document.createElementNS(NS, "circle");
      circle.setAttribute("cx", p.x);
      circle.setAttribute("cy", p.y);
      circle.setAttribute("r", nodeRadius(node, view.centerPath, view.degrees));
      circle.setAttribute("class", "graph-node" + (isCenter ? " graph-node-center" : ""));

      // .textContent, never innerHTML/string concatenation -- every node
      // built via createElementNS + real DOM properties throughout this
      // function, so a document title containing "<"/"&" is inherently
      // inert text, no manual escaping needed (unlike the HTML-string
      // rendering query-block.js does, which DOES need WikiCommon.escapeHtml).
      var label = document.createElementNS(NS, "text");
      label.setAttribute("x", p.x);
      label.setAttribute("y", p.y + (isCenter ? CENTER_LABEL_DY : LABEL_DY));
      label.setAttribute("class", "graph-label");
      label.textContent = nodeLabel(node);
      labelEls.push({ node: node, el: label, link: link });

      link.appendChild(circle);
      link.appendChild(label);
      var titleEl = document.createElementNS(NS, "title");
      titleEl.textContent = nodeLabel(node);
      link.appendChild(titleEl);
      nodesGroup.appendChild(link);
    });
    viewport.appendChild(nodesGroup);
    container.appendChild(svg);

    var camera = cameraFor(graphKey);

    function syncSvgLabels(hoveredPath) {
      var i;
      for (i = 0; i < labelEls.length; i++) {
        var item = labelEls[i];
        var on = showLabel(item.node, view, hoveredPath, camera.scale);
        if (on) item.el.classList.remove("is-hidden");
        else item.el.classList.add("is-hidden");
      }
    }

    attachCamera(svg, camera, width, height, function (hoveredPath) {
      viewport.setAttribute(
        "transform",
        "translate(" + camera.tx + "," + camera.ty + ") scale(" + camera.scale + ")"
      );
      syncSvgLabels(hoveredPath);
    }, {
      hitTest: function (viewPt) {
        return hitNode(nodes, positions, view, viewPt, camera);
      },
      onActivate: navigateTo,
    });
    viewport.setAttribute(
      "transform",
      "translate(" + camera.tx + "," + camera.ty + ") scale(" + camera.scale + ")"
    );
    syncSvgLabels(null);
    live = {
      view: view,
      repaint: function () {
        var i;
        for (i = 0; i < labelEls.length; i++) {
          var item = labelEls[i];
          if (isDimmed(item.node, view, null)) item.link.setAttribute("class", "graph-dim");
          else item.link.removeAttribute("class");
        }
        for (i = 0; i < edgeEls.length; i++) {
          var ee = edgeEls[i];
          var dim = edgeIsDimmed(ee.edge, nodes, view);
          ee.el.setAttribute("class", "graph-edge" + (dim ? " graph-dim" : ""));
        }
        syncSvgLabels(null);
      },
    };
    lastBackend = "svg";
  }

  function paintGraph(container, nodes, edges, positions, width, height, options) {
    live = null;
    container.innerHTML = "";
    var palette = readPalette(container);
    var view = makeView(edges, options);
    var graphKey = options.graphKey || "";
    var forced = options.backend;
    var order;
    if (forced === "webgl" || forced === "canvas" || forced === "svg") {
      order = [forced];
      if (forced !== "canvas") order.push("canvas");
      if (forced !== "svg") order.push("svg");
    } else {
      order = webglFailed ? ["canvas", "svg"] : ["webgl", "canvas", "svg"];
    }

    var i;
    for (i = 0; i < order.length; i++) {
      if (order[i] === "webgl") {
        if (tryWebGL(container, nodes, edges, positions, width, height, palette, view, graphKey)) {
          return;
        }
        container.innerHTML = "";
        continue;
      }
      if (order[i] === "canvas") {
        if (tryCanvas(container, nodes, edges, positions, width, height, palette, view, graphKey)) {
          return;
        }
        container.innerHTML = "";
        continue;
      }
      renderSvg(container, nodes, edges, positions, width, height, view, graphKey);
      return;
    }
  }

  function paintFromCache(job) {
    var fitted = fitPositions(
      layoutCache.positions,
      layoutCache.boxW,
      layoutCache.boxH,
      job.boxW,
      job.boxH
    );
    var opts = job.options || {};
    opts.graphKey = job.key;
    if (!opts.centerPath) opts.centerPath = job.centerPath;
    paintGraph(
      job.container,
      job.nodes,
      job.edges,
      applyPad(fitted, job.pad),
      job.width,
      job.height,
      opts
    );
  }

  // Renders `nodes`/`edges` as a force-directed graph into `container`.
  // `options.centerPath`, if given, marks that one node as "you are
  // here" (larger fill) -- used by the local graph widget; the full
  // graph page omits it. `options.backend` may force "webgl" / "canvas"
  // / "svg" (tests); omitted, we walk the ladder and skip a rung that
  // fails to actually initialize.
  // `options.query` dims non-matching nodes (title/path substring, plus
  // any paths in `options.matchPaths` from FTS content match).
  // `options.labels` is "all" (default, local graph), "hover", or "auto"
  // (hover + hubs + zoomed-in). `options.emptyText` overrides the empty
  // state copy.
  //
  // Layout is async (Worker). Same node/edge set + a new box (resize)
  // scales the cached coordinates instead of simulating again.
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
    var boxW = Math.max(width - 2 * pad, 1);
    var boxH = Math.max(height - 2 * pad, 1);

    if (nodes.length === 0) {
      container.innerHTML = "";
      var empty = document.createElement("p");
      empty.className = "graph-empty";
      empty.textContent = options.emptyText || "No documents to show.";
      container.appendChild(empty);
      lastBackend = null;
      layoutCache = { key: "", positions: null, boxW: 0, boxH: 0 };
      return;
    }

    var key = graphKey(nodes, edges);
    var job = {
      container: container,
      nodes: nodes,
      edges: edges,
      options: options,
      width: width,
      height: height,
      pad: pad,
      boxW: boxW,
      boxH: boxH,
      centerPath: centerPath,
      key: key,
    };

    if (layoutCache.key === key && layoutCache.positions) {
      paintFromCache(job);
      return;
    }

    // Same graph already simulating (typically a resize that landed
    // before the first Worker result) — keep the in-flight run, just
    // remember the latest box to paint into.
    if (inflight && inflight.key === key) {
      inflight.job = job;
      return;
    }

    var gen = ++layoutGen;
    inflight = { gen: gen, key: key, job: job, simW: boxW, simH: boxH };
    requestLayout(nodes, edges, boxW, boxH, gen, function (positions) {
      if (gen !== layoutGen) return;
      var current = inflight && inflight.gen === gen ? inflight.job : job;
      var simW = inflight ? inflight.simW : boxW;
      var simH = inflight ? inflight.simH : boxH;
      inflight = null;
      layoutCache = { key: key, positions: positions, boxW: simW, boxH: simH };
      paintFromCache(current);
    });
  }

  function setView(partial) {
    if (!live || !live.view || typeof live.repaint !== "function") return false;
    if (partial.query !== undefined) {
      live.view.query = String(partial.query || "").trim();
    }
    if (partial.labels !== undefined) {
      var mode = partial.labels;
      if (mode !== "auto" && mode !== "hover" && mode !== "all") mode = "all";
      live.view.labelMode = mode;
    }
    if (partial.matchPaths !== undefined) {
      live.view.matchPaths = partial.matchPaths || {};
    }
    live.repaint();
    return true;
  }

  return {
    layout: function (nodes, edges, width, height) {
      return syncLayout(nodes, edges, width, height);
    },
    render: render,
    setView: setView,
    backend: function () {
      return lastBackend;
    },
  };
})();
