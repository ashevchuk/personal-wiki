// Cosmetic-only fix for a real gap: Toast UI Editor's WYSIWYG canvas and
// markdown-preview tab render `![youtube](url)` with their OWN markdown
// engine, which has zero idea this app's server-side embed convention
// exists (see util/YouTubeEmbed.h / MarkdownRenderer.cpp) — left alone,
// the editor just tries to load the YouTube page URL as an <img> src and
// shows a broken-image icon while editing. This file plugs Toast UI
// Editor's `customHTMLRenderer.image` hook (edit.js wires it in) to show
// a thumbnail + badge instead, so editing looks like what saving+viewing
// actually produces.
//
// Deliberately a SEPARATE, simplified re-implementation of the URL
// parsing in util/YouTubeEmbed.h, not a call into it (there is no way to
// call C++ from here) — and deliberately NOT trying to be as airtight as
// that one. The security-relevant decision (what actually becomes a real
// `<iframe>`) is made once, server-side, on save+view; nothing this file
// decides changes what gets saved or how the final page renders. Worst
// case here is a wrong or missing thumbnail while editing, never a
// security boundary.
window.WikiYouTubeEmbedPreview = (function () {
  "use strict";

  var kIdPattern = /^[A-Za-z0-9_-]{11}$/;

  function extractVideoId(url) {
    if (typeof url !== "string") return null;
    var s = url.replace(/^https?:\/\//i, "").replace(/^(www\.|m\.)/i, "");

    var watchMatch = s.match(/^youtube\.com\/watch(?:\?(.*))?$/);
    if (watchMatch) {
      var query = watchMatch[1] || "";
      var params = query.split("&");
      for (var i = 0; i < params.length; i++) {
        if (params[i].slice(0, 2) === "v=") {
          var value = params[i].slice(2);
          if (kIdPattern.test(value)) return value;
        }
      }
      return null;
    }

    var shortsMatch = s.match(/^youtube\.com\/shorts\/([A-Za-z0-9_-]{11})(?:[/?].*)?$/);
    if (shortsMatch) return shortsMatch[1];

    var embedMatch = s.match(/^youtube\.com\/embed\/([A-Za-z0-9_-]{11})(?:[/?].*)?$/);
    if (embedMatch) return embedMatch[1];

    var shortLinkMatch = s.match(/^youtu\.be\/([A-Za-z0-9_-]{11})(?:[/?].*)?$/);
    if (shortLinkMatch) return shortLinkMatch[1];

    return null;
  }

  // Toast UI Editor v3's customHTMLRenderer contract: `node` is a
  // toastmark AST node (image nodes carry the URL on `.destination` and
  // the alt text as their first child's `.literal`), `context.origin()`
  // returns the library's own default render result for this node so a
  // renderer can fall back to it unchanged. Confirmed this exact shape
  // empirically against the vendored 3.2.2 bundle (a probe page logging
  // both), not assumed from documentation for a version this app doesn't
  // necessarily match exactly.
  //
  // Deliberately returns a SINGLE flat <img> (never a wrapping div/span
  // pair, tried first and reverted) — the markdown-preview tab is plain
  // static HTML and would have rendered a wrapper fine, but the WYSIWYG
  // canvas is a live ProseMirror document, and an inline "image" node in
  // ProseMirror's own schema can only ever BE an <img>, full stop; asking
  // it to render arbitrary child elements silently collapses back down
  // to a bare <img> with just the returned classNames grafted on and
  // everything else (src override included) dropped — confirmed by
  // testing both shapes live rather than assumed. A flat <img> is the
  // one shape both surfaces render identically and correctly, so there's
  // one code path instead of two. The trade-off: no visual "▶ YouTube"
  // badge overlay (CSS ::after doesn't render on replaced elements like
  // <img> either, so a wrapper would be needed for that too) — `alt`/
  // `title` carry the explanation instead, real text either way, just
  // not painted over the thumbnail.
  function customImageRenderer(node, context) {
    var altText =
        node.firstChild && typeof node.firstChild.literal === "string"
            ? node.firstChild.literal
            : "";
    if (altText.toLowerCase() !== "youtube") return context.origin();

    var videoId = extractVideoId(node.destination || "");
    if (!videoId) return context.origin();

    return {
      type: "openTag",
      tagName: "img",
      selfClose: true,
      classNames: ["yt-embed-preview"],
      attributes: {
        src: "https://img.youtube.com/vi/" + videoId + "/mqdefault.jpg",
        alt: "YouTube video (playable after saving)",
        title: "YouTube video " + videoId + " — plays as a real embed once this document is saved",
      },
    };
  }

  return {
    extractVideoId: extractVideoId,
    customImageRenderer: customImageRenderer,
  };
})();
