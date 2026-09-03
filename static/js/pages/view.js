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
          chrome =
            '<p><a href="' +
            basePath() +
            "/edit/" +
            encodeVaultPath(docPath) +
            '">Edit</a> | ' +
            '<button type="button" id="doc-delete-btn" data-path="' +
            escapeHtml(docPath) +
            '">Delete</button></p>';
        }

        container.innerHTML =
          renderBreadcrumbs(docPath) +
          chrome +
          "<h1>" +
          escapeHtml(title) +
          "</h1>" +
          doc.renderedHtml;

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
