// Populates the sidebar's document tree (#nav-tree) and tag list
// (#nav-tags) from the read-only JSON APIs that have existed since M3
// (/api/nav/tree, /api/nav/tags) but had no frontend consumer until now.
// Both endpoints are visibility-gated server-side (see NavRoutes.cpp) —
// this file trusts whatever they return, no client-side filtering.
(function () {
  "use strict";

  function basePath() {
    var meta = document.querySelector('meta[name="wiki-base-path"]');
    return meta ? meta.content : "";
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

  // /api/nav/tree returns a FLAT list of {path, title, visibility} — the
  // folder tree is built here from '/' segments in each path (see
  // NavRoutes.h's doc comment: this is deliberate, left to the consumer).
  function buildTree(container, docs, bp) {
    var root = { children: {}, docs: [] };
    docs.forEach(function (d) {
      var parts = d.path.split("/");
      var node = root;
      for (var i = 0; i < parts.length - 1; i++) {
        node.children[parts[i]] = node.children[parts[i]] || { children: {}, docs: [] };
        node = node.children[parts[i]];
      }
      node.docs.push(d);
    });

    // Folders are purely a client-side grouping of the flat path list —
    // there's no folder entity anywhere in the data model (see
    // NavQueries) — but /folder/{path} (see FolderRoutes.cpp) DOES exist
    // now as a real page listing everything under a prefix, so the label
    // itself is a link there; the separate arrow button only expands/
    // collapses the children in place, without navigating. Expanded by
    // default.
    function render(node, targetEl, pathPrefix) {
      var ul = el("ul");
      Object.keys(node.children)
        .sort()
        .forEach(function (name) {
          var li = el("li");
          var childUl = el("ul", { class: "nav-children" });
          var arrow = el("span", { class: "nav-arrow", text: "▾" });
          var arrowBtn = el("button", { type: "button", class: "nav-arrow-btn" });
          arrowBtn.appendChild(arrow);
          arrowBtn.addEventListener("click", function () {
            var collapsed = childUl.classList.toggle("collapsed");
            arrow.textContent = collapsed ? "▸" : "▾";
          });
          var childPrefix = pathPrefix + name;
          var label = el("a", {
            class: "nav-folder-label",
            href: bp + "/folder/" + encodeVaultPath(childPrefix),
            text: name + "/",
          });
          var wrap = el("span", { class: "nav-folder-btn" });
          wrap.appendChild(arrowBtn);
          wrap.appendChild(label);
          li.appendChild(wrap);
          render(node.children[name], childUl, childPrefix + "/");
          li.appendChild(childUl);
          ul.appendChild(li);
        });
      node.docs
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
          ul.appendChild(li);
        });
      targetEl.appendChild(ul);
    }
    render(root, container, "");
  }

  function buildTags(container, tags, bp) {
    var ul = el("ul");
    tags
      .slice()
      .sort(function (a, b) {
        return b.count - a.count;
      })
      .forEach(function (t) {
        var li = el("li");
        var a = el("a", { href: bp + "/search?tag=" + encodeURIComponent(t.tag) });
        a.textContent = "#" + t.tag + " (" + t.count + ")";
        li.appendChild(a);
        ul.appendChild(li);
      });
    container.appendChild(ul);
  }

  document.addEventListener("DOMContentLoaded", function () {
    var bp = basePath();
    var treeEl = document.getElementById("nav-tree");
    var tagsEl = document.getElementById("nav-tags");

    if (treeEl) {
      fetch(bp + "/api/nav/tree", { credentials: "same-origin" })
        .then(function (r) {
          return r.json();
        })
        .then(function (docs) {
          if (docs.length === 0) {
            treeEl.textContent = "(empty)";
            return;
          }
          buildTree(treeEl, docs, bp);
        })
        .catch(function () {
          treeEl.textContent = "(failed to load)";
        });
    }

    if (tagsEl) {
      fetch(bp + "/api/nav/tags", { credentials: "same-origin" })
        .then(function (r) {
          return r.json();
        })
        .then(function (tags) {
          if (tags.length === 0) {
            tagsEl.textContent = "(none)";
            return;
          }
          buildTags(tagsEl, tags, bp);
        })
        .catch(function () {
          tagsEl.textContent = "(failed to load)";
        });
    }
  });
})();
