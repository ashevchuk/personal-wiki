// Document view page — GET /api/documents/{path}, then breadcrumbs +
// (admin-only) Edit/Delete chrome + the server-rendered markdown HTML.
window.WikiPages = window.WikiPages || {};

(function () {
  "use strict";

  var basePath = WikiCommon.basePath;
  var encodeVaultPath = WikiCommon.encodeVaultPath;
  var escapeHtml = WikiCommon.escapeHtml;
  var renderBreadcrumbs = WikiCommon.renderBreadcrumbs;

  // Local graph widget: this document plus its 1-hop [[wiki-link]]
  // neighbors, rendered the same way the full graph page does (see
  // graph-render.js). Fetches the SAME /api/graph payload the full
  // graph page uses and filters client-side (WikiGraphRender.neighborsOf)
  // rather than a dedicated server-side query — see GraphQueries.h's own
  // comment on why that split isn't worth it at this app's real scale.
  // Omitted entirely (same "don't show an empty section" discipline as
  // the backlinks list right above it) when the document has no
  // neighbors at all — a one-node graph with nothing connected to it
  // isn't worth a widget.
  function renderLocalGraph(docPath) {
    var widgetContainer = document.getElementById("local-graph-container");
    if (!widgetContainer) return;
    fetch(basePath() + "/api/graph", { credentials: "same-origin" })
      .then(function (resp) {
        if (!resp.ok) throw new Error("HTTP " + resp.status);
        return resp.json();
      })
      .then(function (data) {
        var local = window.WikiGraphRender.neighborsOf(docPath, data.nodes, data.edges, 1);
        if (local.nodes.length <= 1) return;  // just this document, no real neighbors
        widgetContainer.innerHTML = '<h3>Local graph</h3><div id="local-graph-svg"></div>';
        widgetContainer.className = "local-graph";
        window.WikiGraphRender.render(document.getElementById("local-graph-svg"),
          local.nodes, local.edges, { width: 500, height: 320, centerPath: docPath });
      })
      .catch(function () {
        // A failed local-graph fetch is cosmetic, not core page content
        // (unlike the document body itself) -- leave the placeholder
        // empty rather than showing an error where a small widget was
        // expected.
      });
  }

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

        // Available to every viewer, not just admin — printing/exporting a
        // PUBLIC document is a plain reader action, no auth implied. Real
        // PDF generation stays entirely client-side: window.print() hands
        // off to the browser's own "Save as PDF" in its print dialog,
        // rather than this app growing a server-side PDF renderer (would
        // be the first HTML-generation the C++ side ever did — see
        // CLAUDE.md's "pure JSON API" architecture rule). The @media
        // print rules in each css/themes/*.css do the actual work of making the
        // output look like a print-out (white background, black text, no
        // neon glow) instead of a screenshot of the terminal theme.
        // Plain .doc-actions — no separate "no-print" marker needed, the
        // print stylesheet already blanket-hides every .doc-actions/
        // .folder-actions button row (Edit/Delete/this one alike; none
        // of them belong in a print-out).
        //
        // Download reuses GET /api/documents/{path}/raw (DocumentRoutes.cpp)
        // — the literal on-disk bytes, front-matter included, same
        // fail-safe-private gating as every other read route. No separate
        // server-side "export" endpoint needed, this is exactly that file.
        var printBar =
          '<div class="doc-actions">' +
          '<button type="button" id="doc-print-btn">Print</button>' +
          '<button type="button" id="doc-download-btn">Download</button>' +
          "</div>";

        var chrome = "";
        if (session.authenticated) {
          // Both actions as same-look buttons in one flex row (see
          // .doc-actions in each css/themes/*.css file) — Edit used to be a bare <a> next
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
            '<a class="btn" href="' +
            basePath() +
            "/history/" +
            encodeVaultPath(docPath) +
            '">History</a>' +
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

        // Every OTHER document that links here via [[wiki-link]] — see
        // NavQueries::backlinks (already visibility-gated server-side,
        // same fail-safe-private rule as everything else: a private
        // linking document never appears to an anonymous viewer). Omit
        // the section entirely rather than showing an empty "Linked
        // from" heading when nothing links here — most documents in a
        // fresh vault won't have any yet.
        var backlinksHtml = "";
        if (doc.backlinks && doc.backlinks.length > 0) {
          backlinksHtml = '<div class="backlinks"><h3>Linked from</h3><ul>';
          doc.backlinks.forEach(function (d) {
            backlinksHtml +=
              '<li><a href="' +
              basePath() +
              "/d/" +
              encodeVaultPath(d.path) +
              '">' +
              escapeHtml(d.title || d.path) +
              "</a>";
            if (d.visibility !== "public") backlinksHtml += " <em>(private)</em>";
            backlinksHtml += "</li>";
          });
          backlinksHtml += "</ul></div>";
        }

        // #doc-body wraps ONLY the document's own title+content, not the
        // breadcrumbs/action-row chrome around it or the backlinks list
        // after it — WikiSectionZoom needs a container scoped to just
        // the zoomable content, or "zoom into this section" would also
        // hide the Edit/Delete buttons and breadcrumb trail along with
        // everything else at the same DOM level.
        container.innerHTML =
          renderBreadcrumbs(docPath) +
          printBar +
          chrome +
          '<div id="doc-body">' +
          titleHtml +
          doc.renderedHtml +
          "</div>" +
          backlinksHtml +
          '<div id="local-graph-container"></div>';

        if (window.WikiMermaid) {
          window.WikiMermaid.renderIn(container);
        }
        if (window.WikiPrismHighlight) {
          window.WikiPrismHighlight.highlightIn(container);
        }
        if (window.WikiQueryBlock) {
          window.WikiQueryBlock.renderIn(container);
        }
        if (window.WikiSectionZoom) {
          window.WikiSectionZoom.setup(container);
        }
        if (window.WikiGraphRender) {
          renderLocalGraph(docPath);
        }

        var deleteBtn = document.getElementById("doc-delete-btn");
        if (deleteBtn && window.WikiDocument) {
          window.WikiDocument.wireDeleteButton(deleteBtn);
        }

        var printBtn = document.getElementById("doc-print-btn");
        if (printBtn) {
          printBtn.addEventListener("click", function () {
            window.print();
          });
        }

        var downloadBtn = document.getElementById("doc-download-btn");
        if (downloadBtn) {
          downloadBtn.addEventListener("click", function () {
            fetch(basePath() + "/api/documents/" + encodeVaultPath(docPath) + "/raw", {
              credentials: "same-origin",
            })
              .then(function (resp) {
                if (!resp.ok) throw new Error("HTTP " + resp.status);
                return resp.blob();
              })
              .then(function (blob) {
                // basename only, not the full vault-relative path -- a
                // path with slashes isn't a valid single filename, and
                // the browser's own Save dialog already lets the user
                // rename/relocate it anyway.
                var filename = docPath.split("/").pop() || "document.md";
                var url = URL.createObjectURL(blob);
                var a = document.createElement("a");
                a.href = url;
                a.download = filename;
                document.body.appendChild(a);
                a.click();
                document.body.removeChild(a);
                URL.revokeObjectURL(url);
              })
              .catch(function () {
                alert("Download failed.");
              });
          });
        }
      })
      .catch(function () {
        container.textContent = "Failed to load document.";
      });
  };
})();
