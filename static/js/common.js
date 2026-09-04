// Shared helpers for every page script (router.js, static/js/pages/*.js,
// nav.js, folder.js, document.js) — a plain global namespace, no bundler/
// module system (this project has no build step by design, see
// CLAUDE.md: every JS file here is loaded as a plain <script>). MUST
// load before any script that uses it.
window.WikiCommon = (function () {
  "use strict";

  // Set by the inline bootstrap script at the top of shell.html's <head>
  // — it infers this deployment's mount prefix (e.g. "/wiki", or "" for
  // an on-root deployment) from location.pathname, before this file (or
  // anything else) even loads. See that script's comment for the full
  // reasoning; this just reuses its result.
  function basePath() {
    return window.__WIKI_BASE_PATH__ || "";
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

  // Mirrors util::escapeHtml (src/util/HtmlEscape.cpp) EXACTLY — same 4
  // characters, same order, no more (notably not the single quote:
  // matches the C++ side, which relies on every attribute in this app
  // being double-quoted, never single-quoted).
  function escapeHtml(str) {
    return String(str)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  // Mirrors SearchRoutes.cpp's renderSnippet exactly, just moved
  // client-side: escape the WHOLE raw snippet first, THEN substitute the
  // FtsSearch match-marker control bytes (ASCII 0x01/0x02) for real
  // <mark>/</mark> tags. Escaping after inserting real tags would mangle
  // them; skipping escaping to preserve them would let the document body
  // through unescaped — the order here is the entire security property,
  // not a stylistic choice. Returns an HTML string, safe to assign to
  // .innerHTML as-is.
  //
  // The snippet is a plain-text excerpt of the RAW markdown body (FTS5's
  // snippet() isn't markdown-aware) — stripped of the most visually noisy
  // markdown syntax (image/link brackets, heading #s) before escaping,
  // purely for readability in a one-line search result; this is NOT a
  // markdown renderer and doesn't need to be one (renderedHtml from
  // GET /api/documents/{path} already exists for that — see view.js).
  // Deliberately NOT stripping *bold*/_italic_ markers too: unlike
  // image/link brackets and a leading heading #, a bare "_" or "*" is
  // genuinely ambiguous with ordinary text (confirmed the hard way —
  // an early version of this function mangled "photo_2026-09-03.jpg"
  // into "photo2026-09-03.jpg", reading the underscores around the date
  // as an italic span). Both regexes below only remove syntax CHARACTERS
  // and always keep whatever text was inside brackets/after # verbatim
  // (via a capture group), so a control byte that landed inside e.g.
  // `![al<mark-here>t](url)` survives intact in the kept text — only one
  // landing inside the discarded `(url)` portion itself would silently
  // lose its highlight, which degrades to "one fewer <mark>", never a
  // broken/mismatched tag.
  function stripMarkdownSyntax(text) {
    return text
      .replace(/!?\[([^\]]*)\]\([^)]*\)/g, "$1")
      // NOT `/^#{1,6}\s+/gm` (start-of-LINE anchored) — confirmed the hard
      // way against a real search snippet: FTS5's snippet() flattens the
      // document's original newlines into plain spaces, so a heading that
      // was on its own line in the source (e.g. "...soup.\n\n## Ingredients\n- Beets...")
      // arrives as "...soup. ## Ingredients - Beets..." with no real "\n"
      // for `^`/`m` to anchor on — `##` sat there raw in every snippet
      // that crossed a heading. Anchoring on "start-of-string OR any
      // whitespace" instead catches both the un-flattened (real editor
      // body) and flattened (FTS5 snippet) cases alike, and still leaves
      // "C#"/"F#" alone (no whitespace immediately before that '#').
      .replace(/(^|\s)#{1,6}\s+/g, "$1")
      // FTS5's snippet() truncates with "..." — if that cut lands INSIDE
      // an image/link's URL portion, the pattern above never sees a
      // closing ')' and doesn't match at all, leaving raw "![alt](https:/..."
      // syntax visible right where the excerpt just happens to end. This
      // catches exactly that: an opened-but-never-closed pattern running
      // to the end of the string, keeping the alt/link text and dropping
      // the truncated URL fragment.
      .replace(/!?\[([^\]]*)\]\([^)]*$/, "$1");
  }

  function markSnippet(rawSnippet, isHighlighted) {
    var escaped = escapeHtml(stripMarkdownSyntax(rawSnippet));
    if (!isHighlighted) return escaped;
    // String.fromCharCode(1)/(2), not literal bytes in the source —
    // a control byte sitting invisibly between two quotes in a source
    // file is a bug waiting to happen the next time someone edits this
    // (confirmed the hard way while writing this very function).
    var startMarker = String.fromCharCode(1);
    var endMarker = String.fromCharCode(2);
    return escaped.split(startMarker).join("<mark>").split(endMarker).join("</mark>");
  }

  // Mirrors util::renderBreadcrumbs (used to live in the now-deleted
  // src/util/PageChrome.cpp): "Home / notes / sub / foo.md" from a
  // vault-relative path. Folder segments are plain text, not links —
  // same reasoning as the C++ version had: only "Home" links anywhere,
  // the trailing segment is styled as the current page/folder. `path`
  // may be empty (renders just "Home"). Returns an HTML string.
  function renderBreadcrumbs(path) {
    var html = '<nav class="breadcrumbs"><a href="' + basePath() + '/">Home</a>';
    var segments = path.split("/").filter(function (s) {
      return s.length > 0;
    });
    segments.forEach(function (seg, i) {
      var isLast = i === segments.length - 1;
      html +=
        ' / <span class="' +
        (isLast ? "crumb-current" : "crumb") +
        '">' +
        escapeHtml(seg) +
        "</span>";
    });
    html += "</nav>";
    return html;
  }

  // API error responses are JSON ({"error": "..."}) — extract the
  // actual message instead of surfacing the raw JSON blob in an
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

  // GET /api/session — {authenticated: bool}. Every page needs this to
  // decide whether to show admin-only chrome (Edit/Delete/New/Rename
  // buttons); centralized here rather than repeated in every page module.
  function fetchSession() {
    return fetch(basePath() + "/api/session", { credentials: "same-origin" }).then(function (r) {
      return r.json();
    });
  }

  return {
    basePath: basePath,
    getCookie: getCookie,
    encodeVaultPath: encodeVaultPath,
    el: el,
    escapeHtml: escapeHtml,
    markSnippet: markSnippet,
    renderBreadcrumbs: renderBreadcrumbs,
    errorFromResponse: errorFromResponse,
    fetchSession: fetchSession,
  };
})();
