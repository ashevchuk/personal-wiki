// Populates the folder browse page (#folder-contents) from the same
// /api/nav/tree endpoint nav.js uses for the sidebar — filtered to direct
// children of this folder's path. No dedicated "list folder contents"
// backend endpoint exists; folders aren't a first-class entity in the
// data model (see NavQueries), so this reuses the one visibility-gated
// source of truth that already exists. Also wires the admin-only New/
// Rename/Delete buttons (present in the DOM only when authenticated —
// see FolderRoutes.cpp's renderFolderPage) to /api/folders/*.
(function () {
  "use strict";

  function basePath() {
    var meta = document.querySelector('meta[name="wiki-base-path"]');
    return meta ? meta.content : "";
  }

  function getCookie(name) {
    var match = document.cookie.match(
      new RegExp("(?:^|; )" + name.replace(/([.$?*|{}()[\]\\/+^])/g, "\\$1") + "=([^;]*)")
    );
    return match ? decodeURIComponent(match[1]) : "";
  }

  function encodeVaultPath(path) {
    return path.split("/").map(encodeURIComponent).join("/");
  }

  function el(tag, attrs) {
    var e = document.createElement(tag);
    if (attrs) {
      Object.keys(attrs).forEach(function (k) {
        if (k === "text") e.textContent = attrs[k];
        else e.setAttribute(k, attrs[k]);
      });
    }
    return e;
  }

  // API errors are JSON ({"error": "..."}) — surface the actual message
  // in an alert() instead of the raw JSON blob.
  function errorFromResponse(resp) {
    return resp.text().then(function (text) {
      try {
        var parsed = JSON.parse(text);
        if (parsed && parsed.error) return new Error(parsed.error);
      } catch (e) {
        // not JSON — fall through
      }
      return new Error(text || "HTTP " + resp.status);
    });
  }

  document.addEventListener("DOMContentLoaded", function () {
    var container = document.getElementById("folder-contents");
    if (!container) return; // not a folder page

    var bp = basePath();
    var folderPath = container.getAttribute("data-folder-path") || "";
    var prefix = folderPath ? folderPath + "/" : "";

    fetch(bp + "/api/nav/tree", { credentials: "same-origin" })
      .then(function (r) {
        return r.json();
      })
      .then(function (docs) {
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

        container.innerHTML = "";
        var folderNames = Object.keys(subfolders).sort();

        var foldersHeading = el("h3", { text: "Folders" });
        container.appendChild(foldersHeading);
        if (folderNames.length === 0) {
          container.appendChild(el("p", { class: "empty", text: "(none)" }));
        } else {
          var folderUl = el("ul", { class: "folder-list" });
          folderNames.forEach(function (name) {
            var li = el("li");
            var a = el("a", { href: bp + "/folder/" + encodeVaultPath(prefix + name) });
            a.textContent = name + "/";
            li.appendChild(a);
            folderUl.appendChild(li);
          });
          container.appendChild(folderUl);
        }

        var docsHeading = el("h3", { text: "Documents" });
        container.appendChild(docsHeading);
        if (directDocs.length === 0) {
          container.appendChild(el("p", { class: "empty", text: "(none directly here)" }));
        } else {
          var docUl = el("ul", { class: "folder-list results" });
          directDocs
            .slice()
            .sort(function (a, b) {
              return a.path.localeCompare(b.path);
            })
            .forEach(function (d) {
              var li = el("li");
              var a = el("a", { href: bp + "/d/" + encodeVaultPath(d.path) });
              a.textContent = d.title || d.path;
              if (d.visibility !== "public") {
                a.appendChild(el("em", { text: " *" }));
              }
              li.appendChild(a);
              docUl.appendChild(li);
            });
          container.appendChild(docUl);
        }
      })
      .catch(function () {
        container.textContent = "(failed to load)";
      });

    var newDocBtn = document.getElementById("folder-new-doc");
    if (newDocBtn) {
      newDocBtn.addEventListener("click", function () {
        var name = window.prompt("New document path (e.g. getting-started.md):");
        if (!name) return;
        window.location.href = bp + "/edit/" + encodeVaultPath(prefix + name.trim());
      });
    }

    var renameBtn = document.getElementById("folder-rename-btn");
    if (renameBtn) {
      renameBtn.addEventListener("click", function () {
        var newPath = window.prompt("Move/rename this folder to:", folderPath);
        if (!newPath || newPath === folderPath) return;
        fetch(bp + "/api/folders/move", {
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
            window.location.href = bp + "/folder/" + encodeVaultPath(newPath);
          })
          .catch(function (err) {
            alert("Move failed: " + err.message);
          });
      });
    }

    var deleteBtn = document.getElementById("folder-delete-btn");
    if (deleteBtn) {
      deleteBtn.addEventListener("click", function () {
        if (!window.confirm("Delete this folder? Only works if it's completely empty.")) return;
        fetch(bp + "/api/folders/" + encodeVaultPath(folderPath), {
          method: "DELETE",
          headers: { "X-CSRF-Token": getCookie("wiki_csrf_token") },
          credentials: "same-origin",
        })
          .then(function (resp) {
            if (!resp.ok) return errorFromResponse(resp).then(function (err) { throw err; });
            var parentIdx = folderPath.lastIndexOf("/");
            var parent = parentIdx === -1 ? "" : folderPath.slice(0, parentIdx);
            window.location.href =
              bp + "/folder" + (parent ? "/" + encodeVaultPath(parent) : "");
          })
          .catch(function (err) {
            alert("Delete failed: " + err.message);
          });
      });
    }
  });
})();
