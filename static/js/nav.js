// Populates the sidebar's document tree (#nav-tree) and tag list
// (#nav-tags) from the read-only JSON APIs that have existed since M3
// (/api/nav/tree, /api/nav/tags) but had no frontend consumer until now.
// Both endpoints are visibility-gated server-side (see NavRoutes.cpp) —
// this file trusts whatever they return, no client-side filtering.
(function () {
  "use strict";

  var basePath = WikiCommon.basePath;
  var encodeVaultPath = WikiCommon.encodeVaultPath;
  var el = WikiCommon.el;

  // Which folder paths are expanded, persisted across the full page
  // reload every navigation in this app does (see router.js's own
  // comment on why — no History API, deliberately). Without this, the
  // tree — collapsed by default on every fresh render — snapped back to
  // fully collapsed on every single click, which made "the sidebar
  // remembers where you were" impossible; a per-browser
  // convenience only, same as sidebar-resize.js's width, never sent
  // anywhere.
  var EXPANDED_KEY = "wiki.expandedFolders";

  function loadExpandedSet() {
    var set = {};
    try {
      var raw = localStorage.getItem(EXPANDED_KEY);
      var arr = raw ? JSON.parse(raw) : [];
      arr.forEach(function (p) {
        set[p] = true;
      });
    } catch (e) {
      // Missing/blocked/corrupt localStorage — every folder just
      // starts collapsed, same as this app's very first-ever load.
    }
    return set;
  }

  function saveExpandedSet(set) {
    try {
      localStorage.setItem(
        EXPANDED_KEY,
        JSON.stringify(
          Object.keys(set).filter(function (p) {
            return set[p];
          })
        )
      );
    } catch (e) {
      // Expansion still works for the rest of THIS page load — it just
      // won't survive a reload.
    }
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
    // collapses the children in place, without navigating.
    //
    // Collapsed by default (every level, not just top) UNLESS this exact
    // folder path is in the persisted expandedSet (see loadExpandedSet
    // above) — a vault with a few hundred documents would otherwise dump
    // its ENTIRE tree open on every single page load, which is exactly
    // the opposite of a nav aid at that size; but a folder the reader
    // deliberately opened should still be open after they click into a
    // document inside it, not snap shut on the very next page load.
    var expandedSet = loadExpandedSet();

    function render(node, targetEl, pathPrefix) {
      var ul = el("ul");
      Object.keys(node.children)
        .sort()
        .forEach(function (name) {
          var li = el("li");
          var childPrefix = pathPrefix + name;
          var isExpanded = !!expandedSet[childPrefix];
          var childUl = el("ul", { class: isExpanded ? "nav-children" : "nav-children collapsed" });
          var arrow = el("span", { class: "nav-arrow", text: isExpanded ? "▾" : "▸" });
          var arrowBtn = el("button", { type: "button", class: "nav-arrow-btn" });
          arrowBtn.appendChild(arrow);
          arrowBtn.addEventListener("click", function () {
            var collapsed = childUl.classList.toggle("collapsed");
            arrow.textContent = collapsed ? "▸" : "▾";
            if (collapsed) delete expandedSet[childPrefix];
            else expandedSet[childPrefix] = true;
            saveExpandedSet(expandedSet);
          });
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

  // /api/nav/tags (NavQueries::tagCounts) already returns tags
  // alphabetically (COLLATE NOCASE) — no client-side re-sort here
  // anymore. There used to be one, by count descending, which fought
  // the server's own order for no reason; an alphabetical list is
  // easier to scan for a specific tag once there are more than a
  // handful, which is the whole point of a nav aid.
  function buildTags(container, tags, bp) {
    var ul = el("ul");
    tags.forEach(function (t) {
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
