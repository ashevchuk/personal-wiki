// Document history page (/history/{path...}) — Phase 2 versioning UI.
// Admin-only (matches the backend: every /api/document-history/ and
// /api/document-restore/ route requires requireAdminApi — see
// VersionRoutes.cpp), so this redirects to /login the same way edit.js
// does rather than rendering a form that would just 401 on first fetch.
window.WikiPages = window.WikiPages || {};

(function () {
  "use strict";

  var basePath = WikiCommon.basePath;
  var getCookie = WikiCommon.getCookie;
  var encodeVaultPath = WikiCommon.encodeVaultPath;
  var escapeHtml = WikiCommon.escapeHtml;
  var errorFromResponse = WikiCommon.errorFromResponse;
  var renderBreadcrumbs = WikiCommon.renderBreadcrumbs;

  // Renders a WikiDiff.diffLines() result as HTML — one line per <div>,
  // colored the same way this app already colors other pass/fail-shaped
  // status text (--ok/--error, see theme.css's #f-status rules), so this
  // doesn't introduce a third color convention for "good"/"bad" text.
  function renderDiff(diffOps) {
    if (diffOps.length === 0) return '<p class="empty">No differences.</p>';
    var html = '<pre class="diff-view">';
    diffOps.forEach(function (op) {
      var prefix = op.type === "add" ? "+ " : op.type === "remove" ? "- " : "  ";
      var cls = op.type === "add" ? "diff-add" : op.type === "remove" ? "diff-remove" : "diff-same";
      html += '<div class="' + cls + '">' + escapeHtml(prefix + op.line) + "</div>";
    });
    html += "</pre>";
    return html;
  }

  window.WikiPages.renderHistory = function (container, docPath, session) {
    if (!session.authenticated) {
      window.location.href = basePath() + "/login";
      return;
    }
    document.getElementById("page-title").textContent = "History — " + docPath + " — wiki";

    container.innerHTML = renderBreadcrumbs(docPath) + "<h1>History</h1><p>Loading&hellip;</p>";

    Promise.all([
      fetch(basePath() + "/api/documents/" + encodeVaultPath(docPath), {
        credentials: "same-origin",
      }).then(function (r) {
        if (!r.ok) throw new Error("HTTP " + r.status);
        return r.json();
      }),
      fetch(basePath() + "/api/document-history/" + encodeVaultPath(docPath), {
        credentials: "same-origin",
      }).then(function (r) {
        if (!r.ok) throw new Error("HTTP " + r.status);
        return r.json();
      }),
    ])
      .then(function (results) {
        var current = results[0];
        var snapshots = results[1].snapshots || [];

        var html =
          renderBreadcrumbs(docPath) +
          "<h1>History — " +
          escapeHtml(current.title || docPath) +
          "</h1>";

        if (snapshots.length === 0) {
          html += '<p class="empty">No past versions yet — history starts recording from the next edit.</p>';
        } else {
          html += '<ul class="history-list">';
          snapshots.forEach(function (s) {
            html +=
              '<li data-snapshot-id="' +
              s.id +
              '"><span class="history-timestamp">' +
              escapeHtml(s.snapshotAt) +
              '</span><span class="history-actions">' +
              '<button type="button" class="history-diff-btn" data-id="' +
              s.id +
              '">View diff</button>' +
              '<button type="button" class="history-restore-btn" data-id="' +
              s.id +
              '">Restore</button>' +
              "</span></li>";
          });
          html += "</ul>";
        }
        html += '<div id="history-diff-container"></div>';

        container.innerHTML = html;

        var diffContainer = document.getElementById("history-diff-container");

        container.querySelectorAll(".history-diff-btn").forEach(function (btn) {
          btn.addEventListener("click", function () {
            var id = btn.getAttribute("data-id");
            diffContainer.innerHTML = "<p>Loading diff&hellip;</p>";
            fetch(
              basePath() + "/api/document-history/" + encodeVaultPath(docPath) + "?id=" + id,
              { credentials: "same-origin" }
            )
              .then(function (r) {
                if (!r.ok) throw new Error("HTTP " + r.status);
                return r.json();
              })
              .then(function (snapshot) {
                var ops = WikiDiff.diffLines(snapshot.body || "", current.body || "");
                diffContainer.innerHTML =
                  "<h3>Diff: this version &rarr; current</h3>" + renderDiff(ops);
              })
              .catch(function () {
                diffContainer.innerHTML = '<p class="empty">Failed to load that version.</p>';
              });
          });
        });

        container.querySelectorAll(".history-restore-btn").forEach(function (btn) {
          btn.addEventListener("click", function () {
            var id = btn.getAttribute("data-id");
            if (
              !window.confirm(
                "Restore this version? The CURRENT content is snapshotted first, so this itself can be undone."
              )
            ) {
              return;
            }
            fetch(
              basePath() +
                "/api/document-restore/" +
                encodeVaultPath(docPath) +
                "?id=" +
                id,
              {
                method: "POST",
                headers: { "X-CSRF-Token": getCookie("wiki_csrf_token") },
                credentials: "same-origin",
              }
            )
              .then(function (resp) {
                if (!resp.ok) return errorFromResponse(resp).then(function (err) { throw err; });
                window.location.href = basePath() + "/d/" + encodeVaultPath(docPath);
              })
              .catch(function (err) {
                alert("Restore failed: " + err.message);
              });
          });
        });
      })
      .catch(function () {
        container.innerHTML =
          renderBreadcrumbs(docPath) + '<p class="empty">Failed to load history.</p>';
      });
  };
})();
