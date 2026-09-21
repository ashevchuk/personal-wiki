// Shared document actions — a small library, not a self-wiring page
// controller (unlike nav.js/folder.js), because it's used from two
// different page modules that each wire it at their own point (after
// they've finished building their own DOM): view.js (Delete +
// Rename/Move) and pages/edit.js (Delete only, shown for an existing
// document). Soft-deletes via DELETE /api/documents/{path} (moves to
// .trash/, see DocumentService::softDelete). Rename/Move posts
// {oldPath, newPath} to POST /api/documents/move (file + .assets/ +
// inbound wiki-link / asset-href rewrite, see DocumentService::rename).
window.WikiDocument = (function () {
  "use strict";

  var basePath = WikiCommon.basePath();
  var getCookie = WikiCommon.getCookie;
  var encodeVaultPath = WikiCommon.encodeVaultPath;
  var errorFromResponse = WikiCommon.errorFromResponse;

  function wireDeleteButton(btn) {
    var path = btn.getAttribute("data-path");
    btn.addEventListener("click", function () {
      WikiDialog.confirm('Delete "' + path + '"? It moves to .trash/, not a permanent erase.', {
        danger: true,
        okLabel: "Delete",
      }).then(function (ok) {
        if (!ok) return;
        fetch(basePath + "/api/documents/" + encodeVaultPath(path), {
          method: "DELETE",
          headers: { "X-CSRF-Token": getCookie("wiki_csrf_token") },
          credentials: "same-origin",
        })
          .then(function (resp) {
            if (!resp.ok) return errorFromResponse(resp).then(function (err) { throw err; });
            var parentIdx = path.lastIndexOf("/");
            var parent = parentIdx === -1 ? "" : path.slice(0, parentIdx);
            window.location.href =
              basePath + "/folder" + (parent ? "/" + encodeVaultPath(parent) : "");
          })
          .catch(function (err) {
            WikiDialog.alert("Delete failed: " + err.message);
          });
      });
    });
  }

  function wireRenameButton(btn) {
    var path = btn.getAttribute("data-path");
    btn.addEventListener("click", function () {
      WikiDialog.prompt("Move/rename this document to:", path).then(function (newPath) {
        if (!newPath || newPath === path) return;
        fetch(basePath + "/api/documents/move", {
          method: "POST",
          headers: {
            "Content-Type": "application/json",
            "X-CSRF-Token": getCookie("wiki_csrf_token"),
          },
          credentials: "same-origin",
          body: JSON.stringify({ oldPath: path, newPath: newPath }),
        })
          .then(function (resp) {
            if (!resp.ok) return errorFromResponse(resp).then(function (err) { throw err; });
            return resp.json();
          })
          .then(function (data) {
            var dest = (data && data.newPath) || newPath;
            window.location.href = basePath + "/d/" + encodeVaultPath(dest);
          })
          .catch(function (err) {
            WikiDialog.alert("Rename failed: " + err.message);
          });
      });
    });
  }

  return { wireDeleteButton: wireDeleteButton, wireRenameButton: wireRenameButton };
})();
