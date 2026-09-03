// Shared "delete this document" behavior — a small library, not a
// self-wiring page controller (unlike nav.js/folder.js), because it's
// used from two different contexts that need to sequence the wiring
// differently: the document view page's own Delete button (wired by a
// tiny inline bootstrap script right in DocumentRoutes.cpp's chrome —
// that page has no dedicated JS file of its own) and the edit page's
// Delete button (wired explicitly from edit.js, after its own setup).
// Soft-deletes via DELETE /api/documents/{path} (moves to .trash/, see
// DocumentService::softDelete) — never a hard, irreversible delete.
window.WikiDocument = (function () {
  "use strict";

  var basePath = WikiCommon.basePath();
  var getCookie = WikiCommon.getCookie;
  var encodeVaultPath = WikiCommon.encodeVaultPath;
  var errorFromResponse = WikiCommon.errorFromResponse;

  function wireDeleteButton(btn) {
    var path = btn.getAttribute("data-path");
    btn.addEventListener("click", function () {
      if (!window.confirm('Delete "' + path + '"? It moves to .trash/, not a permanent erase.')) {
        return;
      }
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
          alert("Delete failed: " + err.message);
        });
    });
  }

  return { wireDeleteButton: wireDeleteButton };
})();
