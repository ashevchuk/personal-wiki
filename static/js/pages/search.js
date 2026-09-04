// Search page — q/tag/type filters, debounced live results. Used to be
// htmx wired to a server-rendered HTML fragment (GET /api/search); now
// that endpoint is JSON, so this file owns rendering entirely — htmx has
// nothing left to do here (its whole point was swapping in server HTML)
// and isn't loaded on this page.
window.WikiPages = window.WikiPages || {};

(function () {
  "use strict";

  var basePath = WikiCommon.basePath;
  var encodeVaultPath = WikiCommon.encodeVaultPath;
  var escapeHtml = WikiCommon.escapeHtml;
  var markSnippet = WikiCommon.markSnippet;

  // Checkbox-list dropdown replacing what used to be a free-text
  // <input name="tag">/<input name="type"> — the user had to already
  // know exact tag/type spelling and type it by hand, with zero
  // discoverability of what actually exists in the vault. Options come
  // from /api/nav/tags and /api/nav/types (both already visibility-gated
  // server-side, same as the sidebar's tag cloud), so this can never
  // offer a choice that would silently 404/empty-result against content
  // the caller isn't allowed to see.
  //
  // Deliberately hand-rolled instead of a native <select multiple> —
  // the native widget's default rendering (a fixed-height listbox
  // needing Ctrl/Cmd-click to multi-pick) doesn't fit this app's
  // hover/keyboard-light UI at all and can't be restyled to match the
  // rest of the theme. A checkbox popover is the standard replacement.
  //
  // opts: [{value, count}], initialSelected: string[]. onChange(selected
  // string[]) fires on every checkbox toggle — no debounce here, unlike
  // the free-text `q` input, since a checkbox click is already a single
  // discrete action, not a keystroke stream.
  function createMultiSelect(mount, label, opts, initialSelected, onChange) {
    var selected = {};
    initialSelected.forEach(function (v) {
      if (v) selected[v] = true;
    });

    var root = document.createElement("div");
    root.className = "ms-select";

    var toggle = document.createElement("button");
    toggle.type = "button";
    toggle.className = "ms-toggle";

    var menu = document.createElement("div");
    menu.className = "ms-menu";
    menu.hidden = true;

    function selectedList() {
      return Object.keys(selected).filter(function (k) {
        return selected[k];
      });
    }

    function updateToggleLabel() {
      var n = selectedList().length;
      toggle.textContent = label + (n > 0 ? " (" + n + ")" : "");
    }

    if (opts.length === 0) {
      var empty = document.createElement("p");
      empty.className = "ms-empty";
      empty.textContent = "None yet.";
      menu.appendChild(empty);
    }
    opts.forEach(function (opt) {
      var row = document.createElement("label");
      var cb = document.createElement("input");
      cb.type = "checkbox";
      cb.value = opt.value;
      cb.checked = !!selected[opt.value];
      cb.addEventListener("change", function () {
        if (cb.checked) selected[opt.value] = true;
        else delete selected[opt.value];
        updateToggleLabel();
        onChange(selectedList());
      });
      row.appendChild(cb);
      row.appendChild(document.createTextNode(" " + opt.value + " "));
      var count = document.createElement("span");
      count.className = "ms-count";
      count.textContent = "(" + opt.count + ")";
      row.appendChild(count);
      menu.appendChild(row);
    });

    toggle.addEventListener("click", function (evt) {
      evt.stopPropagation();
      menu.hidden = !menu.hidden;
    });
    menu.addEventListener("click", function (evt) {
      evt.stopPropagation(); // clicking a checkbox/label shouldn't close the menu
    });
    document.addEventListener("click", function () {
      menu.hidden = true;
    });
    document.addEventListener("keydown", function (evt) {
      if (evt.key === "Escape") menu.hidden = true;
    });

    updateToggleLabel();
    root.appendChild(toggle);
    root.appendChild(menu);
    mount.appendChild(root);
    return { selected: selectedList };
  }

  function renderResults(resultsEl, results) {
    if (results.length === 0) {
      resultsEl.innerHTML = '<p class="empty">No documents found.</p>';
      return;
    }
    var html = '<ul class="results">';
    results.forEach(function (item) {
      html +=
        '<li><a href="' +
        basePath() +
        "/d/" +
        encodeVaultPath(item.path) +
        '">' +
        escapeHtml(item.title || item.path) +
        "</a>";
      if (item.visibility !== "public") html += " <em>(private)</em>";
      html += '<p class="snippet">' + markSnippet(item.snippet, item.snippetIsHighlighted) + "</p>";
      if (item.tags && item.tags.length > 0) {
        html += '<p class="tags">';
        item.tags.forEach(function (t) {
          html += "#" + escapeHtml(t) + " ";
        });
        html += "</p>";
      }
      html += "</li>";
    });
    html += "</ul>";
    resultsEl.innerHTML = html;
  }

  window.WikiPages.renderSearch = function (container) {
    document.getElementById("page-title").textContent = "Search — wiki";

    var params = new URLSearchParams(location.search);
    var qValue = params.get("q") || "";
    // Comma-separated — matches what buildQuery() on the server now
    // splits both params on (SearchRoutes.cpp), and what the multiselect
    // below joins its checked boxes into.
    var tagValues = (params.get("tag") || "").split(",").filter(Boolean);
    var typeValues = (params.get("type") || "").split(",").filter(Boolean);

    container.innerHTML =
      "<h1>Search</h1>" +
      '<form id="search-form" class="search-form" onsubmit="return false">' +
      '<input type="search" name="q" placeholder="Search documents..." value="' +
      escapeHtml(qValue) +
      '" autofocus>' +
      '<div id="tag-select-mount"></div>' +
      '<div id="type-select-mount"></div>' +
      "</form>" +
      '<div id="results">Loading&hellip;</div>';

    var form = document.getElementById("search-form");
    var resultsEl = document.getElementById("results");
    var selectedTags = tagValues;
    var selectedTypes = typeValues;

    function runSearch() {
      var q = new URLSearchParams();
      if (form.q.value) q.set("q", form.q.value);
      if (selectedTags.length > 0) q.set("tag", selectedTags.join(","));
      if (selectedTypes.length > 0) q.set("type", selectedTypes.join(","));
      fetch(basePath() + "/api/search?" + q.toString(), { credentials: "same-origin" })
        .then(function (r) {
          return r.json();
        })
        .then(function (data) {
          renderResults(resultsEl, data.results || []);
        })
        .catch(function () {
          resultsEl.textContent = "Search failed.";
        });
    }

    var debounceTimer = null;
    function debouncedSearch() {
      if (debounceTimer) clearTimeout(debounceTimer);
      debounceTimer = setTimeout(runSearch, 300);
    }
    form.q.addEventListener("input", debouncedSearch);

    // Both option lists are visibility-gated server-side the same way the
    // sidebar's tag cloud is (see NavRoutes.cpp) — an anonymous caller
    // never even sees a private-only tag/type as a choice here, so there's
    // no separate client-side filtering to get wrong.
    Promise.all([
      fetch(basePath() + "/api/nav/tags", { credentials: "same-origin" }).then(function (r) {
        return r.json();
      }),
      fetch(basePath() + "/api/nav/types", { credentials: "same-origin" }).then(function (r) {
        return r.json();
      }),
    ])
      .then(function (results) {
        var tagOpts = results[0].map(function (t) {
          return { value: t.tag, count: t.count };
        });
        var typeOpts = results[1].map(function (t) {
          return { value: t.type, count: t.count };
        });
        createMultiSelect(
          document.getElementById("tag-select-mount"),
          "Tags",
          tagOpts,
          selectedTags,
          function (sel) {
            selectedTags = sel;
            runSearch();
          }
        );
        createMultiSelect(
          document.getElementById("type-select-mount"),
          "Type",
          typeOpts,
          selectedTypes,
          function (sel) {
            selectedTypes = sel;
            runSearch();
          }
        );
      })
      .catch(function () {
        // Filter options failed to load — search by text alone still
        // works fine without them, so this is degraded, not broken.
      });

    runSearch();
  };
})();
