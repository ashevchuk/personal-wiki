// Folder browse page — GET /api/nav/tree filtered to this folder's direct
// children (subfolders + documents), same as the sidebar's tree in
// nav.js; no dedicated "list folder contents" backend endpoint exists,
// folders aren't a first-class entity in the data model (see NavQueries).
// Also owns the admin-only New/Rename/Delete actions
// (POST/DELETE /api/folders/*).
window.WikiPages = window.WikiPages || {};

(function () {
  "use strict";

  var basePath = WikiCommon.basePath;
  var getCookie = WikiCommon.getCookie;
  var encodeVaultPath = WikiCommon.encodeVaultPath;
  var escapeHtml = WikiCommon.escapeHtml;
  var el = WikiCommon.el;
  var errorFromResponse = WikiCommon.errorFromResponse;
  var renderBreadcrumbs = WikiCommon.renderBreadcrumbs;

  // Shared with the sidebar's own "+ New" button (see router.js) — a
  // folder-scoped version just passes a non-empty prefix.
  //
  // Used to window.prompt() for the path FIRST, then land on the
  // already-full form (title/tags/type/visibility/editor — see
  // edit.js::buildForm, which has always rendered all of that for a new
  // document, prompt() or not) with that path baked in. That meant
  // creating a document was a native browser dialog immediately
  // followed by a real form asking for everything else — two different
  // UIs, one right after the other, for one action. Now it's just a
  // navigation straight to the form; edit.js treats an empty/prefix-only
  // path as "new" without a round-trip to the server first (see its own
  // comment), and the Path field there is a normal, editable input
  // pre-filled with `prefix` — typing the filename is now the SAME
  // form field a rename/inspect of the path would use anyway, not a
  // separate one-shot dialog.
  window.WikiPages.newDocument = function (prefix) {
    window.location.href = basePath() + "/edit/" + encodeVaultPath(prefix);
  };

  function renderContents(contentsEl, folderPath, docs) {
    var prefix = folderPath ? folderPath + "/" : "";
    var subfolders = {};
    var directDocs = [];
    docs.forEach(function (d) {
      if (prefix && d.path.indexOf(prefix) !== 0) return;
      var rest = prefix ? d.path.slice(prefix.length) : d.path;
      var parts = rest.split("/");
      if (parts.length === 1) {
        directDocs.push(d);
      } else {
        subfolders[parts[0]] = true;
      }
    });

    contentsEl.innerHTML = "";
    var folderNames = Object.keys(subfolders).sort();

    contentsEl.appendChild(el("h3", { text: "Folders" }));
    if (folderNames.length === 0) {
      contentsEl.appendChild(el("p", { class: "empty", text: "(none)" }));
    } else {
      var folderUl = el("ul", { class: "folder-list" });
      folderNames.forEach(function (name) {
        var li = el("li");
        var a = el("a", { href: basePath() + "/folder/" + encodeVaultPath(prefix + name) });
        a.textContent = name + "/";
        li.appendChild(a);
        folderUl.appendChild(li);
      });
      contentsEl.appendChild(folderUl);
    }

    contentsEl.appendChild(el("h3", { text: "Documents" }));
    if (directDocs.length === 0) {
      contentsEl.appendChild(el("p", { class: "empty", text: "(none directly here)" }));
    } else {
      var docUl = el("ul", { class: "folder-list results" });
      directDocs
        .slice()
        .sort(function (a, b) {
          return a.path.localeCompare(b.path);
        })
        .forEach(function (d) {
          var li = el("li");
          var a = el("a", { href: basePath() + "/d/" + encodeVaultPath(d.path) });
          a.textContent = d.title || d.path;
          if (d.visibility !== "public") a.appendChild(el("em", { text: " *" }));
          li.appendChild(a);
          docUl.appendChild(li);
        });
      contentsEl.appendChild(docUl);
    }
  }

  function wireActions(actionsEl, folderPath) {
    var newBtn = el("button", { type: "button", id: "folder-new-doc", text: "+ New document" });
    newBtn.addEventListener("click", function () {
      window.WikiPages.newDocument(folderPath ? folderPath + "/" : "");
    });
    actionsEl.appendChild(newBtn);

    if (!folderPath) return; // can't rename/delete the vault root

    var renameBtn = el("button", { type: "button", text: "Rename/Move" });
    renameBtn.addEventListener("click", function () {
      var newPath = window.prompt("Move/rename this folder to:", folderPath);
      if (!newPath || newPath === folderPath) return;
      fetch(basePath() + "/api/folders/move", {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          "X-CSRF-Token": getCookie("wiki_csrf_token"),
        },
        credentials: "same-origin",
        body: JSON.stringify({ oldPath: folderPath, newPath: newPath }),
      })
        .then(function (resp) {
          if (!resp.ok) return errorFromResponse(resp).then(function (err) { throw err; });
          window.location.href = basePath() + "/folder/" + encodeVaultPath(newPath);
        })
        .catch(function (err) {
          alert("Move failed: " + err.message);
        });
    });
    actionsEl.appendChild(document.createTextNode(" "));
    actionsEl.appendChild(renameBtn);

    var deleteBtn = el("button", { type: "button", text: "Delete (if empty)" });
    deleteBtn.addEventListener("click", function () {
      if (!window.confirm("Delete this folder? Only works if it's completely empty.")) return;
      fetch(basePath() + "/api/folders/" + encodeVaultPath(folderPath), {
        method: "DELETE",
        headers: { "X-CSRF-Token": getCookie("wiki_csrf_token") },
        credentials: "same-origin",
      })
        .then(function (resp) {
          if (!resp.ok) return errorFromResponse(resp).then(function (err) { throw err; });
          var parentIdx = folderPath.lastIndexOf("/");
          var parent = parentIdx === -1 ? "" : folderPath.slice(0, parentIdx);
          window.location.href =
            basePath() + "/folder" + (parent ? "/" + encodeVaultPath(parent) : "");
        })
        .catch(function (err) {
          alert("Delete failed: " + err.message);
        });
    });
    actionsEl.appendChild(document.createTextNode(" "));
    actionsEl.appendChild(deleteBtn);
  }

  window.WikiPages.renderFolder = function (container, folderPath, session) {
    var escapedPath = escapeHtml(folderPath);
    document.getElementById("page-title").textContent =
      (folderPath || "Browse") + " — wiki";

    var heading = folderPath ? escapedPath + "/" : "All documents";
    container.innerHTML =
      renderBreadcrumbs(folderPath) +
      "<h1>" +
      heading +
      '</h1><div class="folder-actions" id="folder-actions"></div>' +
      '<div id="folder-contents">Loading&hellip;</div>';

    if (session.authenticated) {
      wireActions(document.getElementById("folder-actions"), folderPath);
    }

    var contentsEl = document.getElementById("folder-contents");
    fetch(basePath() + "/api/nav/tree", { credentials: "same-origin" })
      .then(function (r) {
        return r.json();
      })
      .then(function (docs) {
        renderContents(contentsEl, folderPath, docs);
      })
      .catch(function () {
        contentsEl.textContent = "(failed to load)";
      });
  };
})();
