// Document view page — GET /api/documents/{path}, then breadcrumbs +
// (admin-only) Edit/Delete chrome + the server-rendered markdown HTML.
window.WikiPages = window.WikiPages || {};

(function () {
  "use strict";

  var basePath = WikiCommon.basePath;
  var encodeVaultPath = WikiCommon.encodeVaultPath;
  var escapeHtml = WikiCommon.escapeHtml;
  var renderBreadcrumbs = WikiCommon.renderBreadcrumbs;

  window.WikiPages.renderView = function (container, docPath, session) {
    fetch(basePath() + "/api/documents/" + encodeVaultPath(docPath), {
      credentials: "same-origin",
    })
      .then(function (resp) {
        if (resp.status === 404) {
          container.innerHTML =
            renderBreadcrumbs(docPath) + "<p>Document not found.</p>";
          document.getElementById("page-title").textContent = "Not found — wiki";
          return null;
        }
        if (!resp.ok) throw new Error("HTTP " + resp.status);
        return resp.json();
      })
      .then(function (doc) {
        if (!doc) return;
        var title = doc.title || "(untitled)";
        document.getElementById("page-title").textContent = title + " — wiki";

        var chrome = "";
        if (session.authenticated) {
          // Both actions as same-look buttons in one flex row (see
          // .doc-actions in theme.css) — Edit used to be a bare <a> next
          // to a boxed Delete <button>, split by a literal "|", which
          // read as two different UI languages sharing one line for no
          // reason. <a class="btn"> makes Edit LOOK like a button while
          // still being a real link (no JS needed to navigate there).
          chrome =
            '<div class="doc-actions"><a class="btn" href="' +
            basePath() +
            "/edit/" +
            encodeVaultPath(docPath) +
            '">Edit</a>' +
            '<button type="button" id="doc-delete-btn" data-path="' +
            escapeHtml(docPath) +
            '">Delete</button></div>';
        }

        // Documents are written with the title as their own first line
        // ("# Title", per this app's own editing convention — see
        // edit.js/every seeded doc) — rendering the front-matter title
        // as a SECOND, separate <h1> on top of that produced a visibly
        // duplicated heading ("Welcome" then "Welcome to the wiki" right
        // under it). Only fall back to the front-matter title as the
        // page's <h1> when the body doesn't already open with one.
        var bodyHasOwnH1 = /^\s*<h1[\s>]/i.test(doc.renderedHtml || "");
        var titleHtml = bodyHasOwnH1 ? "" : "<h1>" + escapeHtml(title) + "</h1>";

        container.innerHTML =
          renderBreadcrumbs(docPath) + chrome + titleHtml + doc.renderedHtml;

        var deleteBtn = document.getElementById("doc-delete-btn");
        if (deleteBtn && window.WikiDocument) {
          window.WikiDocument.wireDeleteButton(deleteBtn);
        }
      })
      .catch(function () {
        container.textContent = "Failed to load document.";
      });
  };
})();
