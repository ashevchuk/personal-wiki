// Barnes-Hut force layout for the graph view. Loaded twice on purpose:
// as a normal <script> (sync fallback, WikiGraphLayout.layout) AND as a
// dedicated Worker (graph-render.js) so a large vault's O(n log n)
// simulation does not freeze the UI thread. Same file, no bundler —
// the IIFE attaches to `self`, which is window in a page and the worker
// global in a Worker. See graph-render.js for why resize does NOT come
// through here (cached positions are scaled in the renderer instead).
(function (g) {
  "use strict";

  var REPULSION = 6000;
  var SPRING = 0.02;
  var SPRING_LENGTH = 150;
  var DAMPING = 0.85;
  var CENTER_PULL = 0.01;
  var ITERATIONS = 250;
  // s/d threshold: higher is cheaper/rougher. 0.7 is the usual
  // "looks like the exact n² graph at this app's densities" value.
  var THETA = 0.7;
  // Coincident points would otherwise subdivide forever.
  var MAX_DEPTH = 18;

  function Quad(x, y, size) {
    this.x = x;
    this.y = y;
    this.size = size;
    this.mass = 0;
    this.cx = 0;
    this.cy = 0;
    this.index = -1;
    this.children = null;
  }

  // One pool for the whole layout() call: 250 iterations of a 400-node
  // graph would otherwise allocate ~a million short-lived Quads and
  // spend more time in GC than in the actual O(n log n) walk.
  var pool = [];
  var poolN = 0;

  function allocQuad(x, y, size) {
    var q;
    if (poolN < pool.length) {
      q = pool[poolN++];
      q.x = x;
      q.y = y;
      q.size = size;
      q.mass = 0;
      q.cx = 0;
      q.cy = 0;
      q.index = -1;
      q.children = null;
      return q;
    }
    q = new Quad(x, y, size);
    pool.push(q);
    poolN++;
    return q;
  }

  function resetPool() {
    poolN = 0;
  }

  function subdivide(q) {
    var h = q.size / 2;
    q.children = [
      allocQuad(q.x, q.y, h),
      allocQuad(q.x + h, q.y, h),
      allocQuad(q.x, q.y + h, h),
      allocQuad(q.x + h, q.y + h, h),
    ];
  }

  function childIndex(q, x, y) {
    var hx = q.x + q.size / 2;
    var hy = q.y + q.size / 2;
    return (x < hx ? 0 : 1) + (y < hy ? 0 : 2);
  }

  // COM is computed once after every body is in the tree, not on every
  // insert: walking the same ancestors n times was the dominant cost.
  function computeMass(q) {
    if (!q.children) return;
    var m = 0;
    var cx = 0;
    var cy = 0;
    var i;
    for (i = 0; i < 4; i++) {
      computeMass(q.children[i]);
      var c = q.children[i];
      if (!c.mass) continue;
      m += c.mass;
      cx += c.cx * c.mass;
      cy += c.cy * c.mass;
    }
    q.mass = m;
    q.index = -1;
    if (m > 0) {
      q.cx = cx / m;
      q.cy = cy / m;
    }
  }

  function insert(q, bodies, i, depth) {
    var b = bodies[i];
    if (!q.children && q.index < 0 && q.mass === 0) {
      q.index = i;
      q.mass = 1;
      q.cx = b.x;
      q.cy = b.y;
      return;
    }
    if (!q.children) {
      if (depth >= MAX_DEPTH) {
        var piled = q.mass + 1;
        q.cx = (q.cx * q.mass + b.x) / piled;
        q.cy = (q.cy * q.mass + b.y) / piled;
        q.mass = piled;
        q.index = -1;
        return;
      }
      subdivide(q);
      if (q.index >= 0) {
        var old = q.index;
        q.index = -1;
        q.mass = 0;
        insert(q.children[childIndex(q, bodies[old].x, bodies[old].y)], bodies, old, depth + 1);
      }
    }
    insert(q.children[childIndex(q, b.x, b.y)], bodies, i, depth + 1);
  }

  function accumulate(q, body, skipIndex) {
    if (!q.mass) return;
    if (!q.children && q.index === skipIndex) {
      // A MAX_DEPTH pile of coincident points has index -1; a true
      // singleton leaf is the body itself and must not self-repel.
      return;
    }

    var dx = body.x - q.cx;
    var dy = body.y - q.cy;
    var distSq = dx * dx + dy * dy || 0.01;
    var dist = Math.sqrt(distSq);

    // Far enough (or a leaf): treat the cell as one body of mass
    // q.mass. Near a cluster, recurse. Same REPULSION / distSq shape
    // the old pairwise loop used, so a small graph still settles
    // recognizably like it did before.
    if (!q.children || q.size / dist < THETA) {
      var force = (REPULSION * q.mass) / distSq;
      body.vx += (dx / dist) * force;
      body.vy += (dy / dist) * force;
      return;
    }
    var c;
    for (c = 0; c < 4; c++) accumulate(q.children[c], body, skipIndex);
  }

  // Exact pairwise for tiny graphs: BH tree overhead dominates, and
  // this is the local-graph case (a document's connected component).
  var EXACT_N = 32;

  function accumulateExact(bodies, i) {
    var body = bodies[i];
    var j;
    for (j = 0; j < bodies.length; j++) {
      if (j === i) continue;
      var o = bodies[j];
      var dx = body.x - o.x;
      var dy = body.y - o.y;
      var distSq = dx * dx + dy * dy || 0.01;
      var dist = Math.sqrt(distSq);
      var force = REPULSION / distSq;
      body.vx += (dx / dist) * force;
      body.vy += (dy / dist) * force;
    }
  }

  function rootBounds(bodies) {
    var minX = Infinity;
    var minY = Infinity;
    var maxX = -Infinity;
    var maxY = -Infinity;
    var i;
    for (i = 0; i < bodies.length; i++) {
      var b = bodies[i];
      if (b.x < minX) minX = b.x;
      if (b.y < minY) minY = b.y;
      if (b.x > maxX) maxX = b.x;
      if (b.y > maxY) maxY = b.y;
    }
    var size = Math.max(maxX - minX, maxY - minY) || 1;
    size *= 1.02;
    var cx = (minX + maxX) / 2;
    var cy = (minY + maxY) / 2;
    return allocQuad(cx - size / 2, cy - size / 2, size);
  }

  function layout(nodes, edges, width, height) {
    var n = nodes.length;
    var positions = {};
    if (n === 0) return positions;

    var bodies = [];
    var byPath = {};
    var i;
    for (i = 0; i < n; i++) {
      var angle = (2 * Math.PI * i) / n;
      var radius = Math.min(width, height) / 3;
      var body = {
        path: nodes[i].path,
        x: width / 2 + radius * Math.cos(angle),
        y: height / 2 + radius * Math.sin(angle),
        vx: 0,
        vy: 0,
      };
      bodies.push(body);
      byPath[body.path] = body;
    }

    var midX = width / 2;
    var midY = height / 2;
    var iter;
    var e;
    var useExact = n <= EXACT_N;
    var degree = {};
    for (e = 0; e < edges.length; e++) {
      degree[edges[e].source] = (degree[edges[e].source] || 0) + 1;
      degree[edges[e].target] = (degree[edges[e].target] || 0) + 1;
    }
    for (iter = 0; iter < ITERATIONS; iter++) {
      if (!useExact) {
        resetPool();
        var root = rootBounds(bodies);
        for (i = 0; i < n; i++) insert(root, bodies, i, 0);
        computeMass(root);
        for (i = 0; i < n; i++) accumulate(root, bodies[i], i);
      } else {
        for (i = 0; i < n; i++) accumulateExact(bodies, i);
      }

      for (e = 0; e < edges.length; e++) {
        var pa = byPath[edges[e].source];
        var pb = byPath[edges[e].target];
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

      for (i = 0; i < n; i++) {
        var p = bodies[i];
        // Isolates used to share the same pull as a linked cluster, so
        // a vault of mostly-unlinked notes collapsed into one unreadable
        // pile on top of the real structure. A much weaker pull leaves
        // them near the starting circle (the periphery) instead.
        var pull = (degree[p.path] || 0) === 0 ? CENTER_PULL * 0.12 : CENTER_PULL;
        p.vx += (midX - p.x) * pull;
        p.vy += (midY - p.y) * pull;
        p.vx *= DAMPING;
        p.vy *= DAMPING;
        p.x += p.vx;
        p.y += p.vy;
      }
    }

    for (i = 0; i < n; i++) {
      positions[bodies[i].path] = { x: bodies[i].x, y: bodies[i].y };
    }
    return positions;
  }

  g.WikiGraphLayout = { layout: layout };

  if (typeof importScripts === "function") {
    g.onmessage = function (ev) {
      var msg = ev.data || {};
      try {
        var positions = layout(
          msg.nodes || [],
          msg.edges || [],
          msg.width || 1,
          msg.height || 1
        );
        g.postMessage({ id: msg.id, ok: true, positions: positions });
      } catch (err) {
        g.postMessage({
          id: msg.id,
          ok: false,
          error: String((err && err.message) || err),
        });
      }
    };
  }
})(typeof self !== "undefined" ? self : window);
