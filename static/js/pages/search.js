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
    var tagValue = params.get("tag") || "";
    var typeValue = params.get("type") || "";

    container.innerHTML =
      "<h1>Search</h1>" +
      '<form id="search-form" class="search-form" onsubmit="return false">' +
      '<input type="search" name="q" placeholder="Search documents..." value="' +
      escapeHtml(qValue) +
      '" autofocus>' +
      '<input type="text" name="tag" placeholder="tag" value="' +
      escapeHtml(tagValue) +
      '">' +
      '<input type="text" name="type" placeholder="type" value="' +
      escapeHtml(typeValue) +
      '">' +
      "</form>" +
      '<div id="results">Loading&hellip;</div>';

    var form = document.getElementById("search-form");
    var resultsEl = document.getElementById("results");

    function runSearch() {
      var q = new URLSearchParams();
      if (form.q.value) q.set("q", form.q.value);
      if (form.tag.value) q.set("tag", form.tag.value);
      if (form.type.value) q.set("type", form.type.value);
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
    ["q", "tag", "type"].forEach(function (name) {
      form[name].addEventListener("input", debouncedSearch);
    });

    runSearch();
  };
})();
