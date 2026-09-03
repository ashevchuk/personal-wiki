// Shared helpers for every other page script (edit.js, nav.js, folder.js,
// document.js) — a plain global namespace, no bundler/module system (this
// project has no build step by design, see CLAUDE.md: every JS file here
// is loaded as a plain <script>). MUST load before any script that uses
// it — see the script order in util::PageChrome::renderPage and the two
// CSP views that duplicate its structure.
window.WikiCommon = (function () {
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

  // Encodes each path segment individually so legitimate '/' separators
  // survive while everything else in a segment gets properly escaped.
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

  // This app's API error responses are JSON ({"error": "..."}) — extract
  // the actual message instead of surfacing the raw JSON blob in an
  // alert()/status line.
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

  return {
    basePath: basePath,
    getCookie: getCookie,
    encodeVaultPath: encodeVaultPath,
    el: el,
    errorFromResponse: errorFromResponse,
  };
})();
