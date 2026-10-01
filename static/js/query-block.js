// Renders ```query fenced blocks (src/util/MarkdownRenderer.cpp's own
// substituteQueryBlocks turns them into <pre class="query"> holding the
// raw DSL text -- see that function's comment) as a live table, on the
// document VIEW page. GET /api/query re-runs the query (src/index/
// QueryBlocks.cpp, src/controllers/QueryRoutes.cpp) on every page load,
// server-side, visibility-gated exactly the same fail-safe-private way
// as search/nav -- this file never sees or filters raw document data
// itself, it only renders whatever rows the server already decided this
// caller is allowed to see.
//
// No lazy-loaded library here (unlike mermaid.js/Prism.js) -- this is a
// small fetch() + <table> render, nothing worth deferring.
window.WikiQueryBlock = (function () {
  "use strict";

  var basePath = WikiCommon.basePath;
  var escapeHtml = WikiCommon.escapeHtml;
  var encodeVaultPath = WikiCommon.encodeVaultPath;

  function isExternalUrl(path) {
    return /^https?:\/\//i.test(path);
  }

  // "3h ago", not the raw "2026-10-01T10:09:37Z" -- these tables exist to
  // show recency (the `sort: updated` key this same DSL already has), and
  // a relative label is what that's actually for; the exact timestamp
  // isn't lost, just demoted to a hover (`title`) for whoever wants it.
  // Falls back to a bare date once a row is old enough that "14d ago"
  // stops being more useful than just naming the day.
  var MINUTE_MS = 60 * 1000;
  var HOUR_MS = 60 * MINUTE_MS;
  var DAY_MS = 24 * HOUR_MS;
  function formatUpdated(iso) {
    if (!iso) return "";
    var then = new Date(iso);
    if (isNaN(then.getTime())) return escapeHtml(iso);
    var diffMs = Date.now() - then.getTime();
    var label;
    if (diffMs < MINUTE_MS) {
      label = "just now";
    } else if (diffMs < HOUR_MS) {
      label = Math.floor(diffMs / MINUTE_MS) + "m ago";
    } else if (diffMs < DAY_MS) {
      label = Math.floor(diffMs / HOUR_MS) + "h ago";
    } else if (diffMs < 30 * DAY_MS) {
      label = Math.floor(diffMs / DAY_MS) + "d ago";
    } else {
      label = iso.slice(0, 10);
    }
    return (
      '<span title="' + escapeHtml(iso) + '">' + escapeHtml(label) + "</span>"
    );
  }

  function renderTable(rows) {
    if (rows.length === 0) {
      return '<p class="query-empty">No matching documents.</p>';
    }
    var html =
      '<table class="query-results"><thead><tr>' +
      "<th>Title</th><th>Tags</th><th>Updated</th>" +
      "</tr></thead><tbody>";
    for (var i = 0; i < rows.length; i++) {
      var row = rows[i];
      // `links: true` rows carry the external URL itself in `path` (see
      // QueryBlocks.cpp's own comment on that key) -- link straight to
      // it, not through this app's own /d/{path} document-view route.
      // rel="noopener noreferrer" since this leaves the site entirely,
      // same discipline any other outbound link on the page would want.
      var href = isExternalUrl(row.path)
        ? escapeHtml(row.path)
        : basePath() + "/d/" + encodeVaultPath(row.path);
      var relAttr = isExternalUrl(row.path)
        ? ' target="_blank" rel="noopener noreferrer"'
        : "";
      html +=
        '<tr><td><a href="' +
        href +
        '"' +
        relAttr +
        ">" +
        escapeHtml(row.title || row.path) +
        "</a></td><td>" +
        escapeHtml(row.tags || "") +
        "</td><td>" +
        formatUpdated(row.updatedAt) +
        "</td></tr>";
    }
    html += "</tbody></table>";
    return html;
  }

  function replaceWithMessage(node, className, text) {
    if (!node.parentNode) return;
    var wrapper = document.createElement("div");
    wrapper.className = className;
    wrapper.textContent = text;
    node.parentNode.replaceChild(wrapper, node);
  }

  function renderOne(node) {
    var queryText = node.textContent;
    fetch(basePath() + "/api/query?q=" + encodeURIComponent(queryText), {
      credentials: "same-origin",
    })
      .then(function (resp) {
        return resp.json().then(function (body) {
          if (!resp.ok) throw new Error(body.error || "HTTP " + resp.status);
          return body;
        });
      })
      .then(function (body) {
        // node.parentNode can already be null here -- query-editor-preview.js
        // calls renderIn() repeatedly (debounced, on every editor change), and
        // a slow fetch can resolve after Toast UI has already regenerated the
        // Preview panel and discarded this exact node in favor of a fresh one.
        // The one-shot document VIEW page never hits this (renderIn runs once,
        // right after load, on nodes that stay put), which is why this wasn't
        // needed before the editor started reusing the same render path.
        if (!node.parentNode) return;
        var wrapper = document.createElement("div");
        wrapper.className = "query-block";
        wrapper.innerHTML = renderTable(body.rows);
        node.parentNode.replaceChild(wrapper, node);
      })
      .catch(function (err) {
        replaceWithMessage(node, "query-block query-error", "Query error: " + err.message);
      });
  }

  // Renders every pre.query element inside `container` in place. No-op
  // if the container has none.
  function renderIn(container) {
    var nodes = container.querySelectorAll("pre.query");
    for (var i = 0; i < nodes.length; i++) {
      renderOne(nodes[i]);
    }
  }

  return { renderIn: renderIn };
})();
