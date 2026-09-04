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
  // Same idea, separate key — a folder the reader opened and a tag
  // namespace group the reader opened are unrelated pieces of state;
  // sharing one key would mean expanding "notes/" in the document tree
  // and "notes/" as a tag-namespace prefix couldn't be independently
  // remembered, an accidental coupling with no reason behind it.
  var EXPANDED_TAGS_KEY = "wiki.expandedTagGroups";

  function loadExpandedSet(key) {
    var set = {};
    try {
      var raw = localStorage.getItem(key);
      var arr = raw ? JSON.parse(raw) : [];
      arr.forEach(function (p) {
        set[p] = true;
      });
    } catch (e) {
      // Missing/blocked/corrupt localStorage — every folder/group just
      // starts collapsed, same as this app's very first-ever load.
    }
    return set;
  }

  function saveExpandedSet(key, set) {
    try {
      localStorage.setItem(
        key,
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
    var expandedSet = loadExpandedSet(EXPANDED_KEY);

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
            saveExpandedSet(EXPANDED_KEY, expandedSet);
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

  // /api/nav/tags (NavQueries::tagCounts) returns a FLAT list,
  // alphabetically sorted (COLLATE NOCASE) — no client-side re-sort here
  // anymore (there used to be one, by count descending, which fought the
  // server's own order for no reason). A tag containing '/'
  // (e.g. "lang/cpp", "project/wiki") is grouped into a collapsible tree
  // HERE, client-side, on exactly the same '/'-split convention buildTree
  // above already uses for document folders — a namespaced flat list
  // gets just as unusable past a few dozen entries as a flat document
  // list did, so it gets the same fix instead of a second, different nav
  // idiom invented for what's structurally the same problem. A tag with
  // no '/' stays a plain root-level leaf, unchanged from before.
  //
  // `filterQuery`, when non-empty, prunes to leaves whose FULL tag
  // string contains it (case-insensitive) and force-expands every group
  // that survives pruning, regardless of that group's persisted collapse
  // state — a search-time reveal isn't "the reader chose to leave this
  // open", so it's never written to localStorage either way. Cheap
  // enough to rebuild the whole tree from scratch on every keystroke at
  // realistic tag-count scale — same call this app already made for
  // quick-open.js's own filtered list, no debounce there either.
  function buildTags(container, tags, bp, filterQuery) {
    var root = { children: {}, tags: [] };
    tags.forEach(function (t) {
      var parts = t.tag.split("/");
      var node = root;
      for (var i = 0; i < parts.length - 1; i++) {
        node.children[parts[i]] = node.children[parts[i]] || { children: {}, tags: [] };
        node = node.children[parts[i]];
      }
      node.tags.push(t);
    });

    var q = (filterQuery || "").trim().toLowerCase();
    var expandedSet = loadExpandedSet(EXPANDED_TAGS_KEY);

    function matches(fullTag) {
      return !q || fullTag.toLowerCase().indexOf(q) !== -1;
    }

    // A group survives the filter if ANY leaf anywhere underneath it
    // (at any depth) matches — recursive, so a match three levels down
    // still keeps every ancestor group visible (and, below, expanded).
    function nodeHasMatch(node) {
      if (node.tags.some(function (t) { return matches(t.tag); })) return true;
      return Object.keys(node.children).some(function (name) {
        return nodeHasMatch(node.children[name]);
      });
    }

    // Own count plus every descendant's — a collapsed "lang/ (14)" tells
    // you something useful without having to open it first.
    function totalCount(node) {
      var sum = node.tags.reduce(function (acc, t) {
        return acc + t.count;
      }, 0);
      Object.keys(node.children).forEach(function (name) {
        sum += totalCount(node.children[name]);
      });
      return sum;
    }

    function render(node, targetEl, pathPrefix) {
      var ul = el("ul");
      Object.keys(node.children)
        .sort()
        .forEach(function (name) {
          var childNode = node.children[name];
          if (!nodeHasMatch(childNode)) return;

          var childPrefix = pathPrefix + name;
          var forceOpen = q.length > 0;
          var isExpanded = forceOpen || !!expandedSet[childPrefix];
          var childUl = el("ul", {
            class: isExpanded ? "nav-children" : "nav-children collapsed",
          });
          var arrow = el("span", { class: "nav-arrow", text: isExpanded ? "▾" : "▸" });
          var label = el("span", {
            text: name + "/ (" + totalCount(childNode) + ")",
          });
          // The WHOLE row toggles here, unlike buildTree's document
          // folders (separate arrow-button vs. label-link) — a tag
          // namespace group has no equivalent of /folder/{path} to
          // navigate a label to, so there's no second action competing
          // for the click; making the whole row the target is just a
          // bigger, easier-to-hit toggle instead of the narrow arrow
          // glyph alone.
          var btn = el("button", { type: "button", class: "nav-tag-group-btn" });
          btn.appendChild(arrow);
          btn.appendChild(label);
          btn.addEventListener("click", function () {
            var collapsed = childUl.classList.toggle("collapsed");
            arrow.textContent = collapsed ? "▸" : "▾";
            if (collapsed) delete expandedSet[childPrefix];
            else expandedSet[childPrefix] = true;
            saveExpandedSet(EXPANDED_TAGS_KEY, expandedSet);
          });
          var li = el("li");
          li.appendChild(btn);
          render(childNode, childUl, childPrefix + "/");
          li.appendChild(childUl);
          ul.appendChild(li);
        });

      node.tags
        .filter(function (t) {
          return matches(t.tag);
        })
        .forEach(function (t) {
          var li = el("li");
          var a = el("a", { href: bp + "/search?tag=" + encodeURIComponent(t.tag) });
          // Under a group, show just the leaf's own last segment
          // ("cpp"), not the full "lang/cpp" — the group header above it
          // already established the prefix, same as the document tree
          // only ever shows a filename once inside a folder, never the
          // whole path again.
          var leafName = t.tag.slice(pathPrefix.length);
          a.textContent = "#" + leafName + " (" + t.count + ")";
          li.appendChild(a);
          ul.appendChild(li);
        });

      targetEl.appendChild(ul);
    }

    container.innerHTML = "";
    render(root, container, "");
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
          // The filter input and the rendered tree are separate
          // elements — buildTags() does `container.innerHTML = ""` on
          // every re-render (see its own comment on why that's fine at
          // this scale), which would wipe the input itself right along
          // with the list if it lived in the same container.
          var filterInput = el("input", {
            type: "text",
            class: "nav-tags-filter",
            placeholder: "Filter tags…",
            autocomplete: "off",
          });
          var listEl = el("div", { class: "nav-tags-list" });
          tagsEl.appendChild(filterInput);
          tagsEl.appendChild(listEl);
          buildTags(listEl, tags, bp, "");
          filterInput.addEventListener("input", function () {
            buildTags(listEl, tags, bp, filterInput.value);
          });
        })
        .catch(function () {
          tagsEl.textContent = "(failed to load)";
        });
    }
  });
})();
