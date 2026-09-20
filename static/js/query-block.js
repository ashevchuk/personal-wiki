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
      html +=
        "<tr><td><a href=\"" +
        basePath() +
        "/d/" +
        encodeVaultPath(row.path) +
        '">' +
        escapeHtml(row.title || row.path) +
        "</a></td><td>" +
        escapeHtml(row.tags || "") +
        "</td><td>" +
        escapeHtml(row.updatedAt || "") +
        "</td></tr>";
    }
    html += "</tbody></table>";
    return html;
  }

  function replaceWithMessage(node, className, text) {
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
