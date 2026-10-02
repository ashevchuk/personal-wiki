#include "util/MarkdownRenderer.h"

#include "util/YouTubeEmbed.h"

#include <md4c-html.h>

#include <stdexcept>

namespace wikicore::util {

namespace {

void appendOutput(const MD_CHAR* text, MD_SIZE size, void* userdata) {
  static_cast<std::string*>(userdata)->append(text, size);
}

bool isYouTubeIdChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
         c == '_' || c == '-';
}

// Swaps every `<img src="youtube-embed:ID" alt="">` md4c produced for a
// real, narrowly-templated `<iframe>`. That exact HTML shape normally
// comes from the `![](youtube-embed:ID)` marker YouTubeEmbed::
// rewriteYouTubeEmbeds writes after recognizing a real youtube.com/
// youtu.be URL — but it is NOT the only way to reach it: a raw
// `<img src="youtube-embed:ID">` typed by hand as literal HTML DOES get
// escaped to `&lt;img ...&gt;` by md4c (MD_FLAG_NOHTMLSPANS is on below,
// confirmed live), but plain CommonMark image syntax typed directly —
// `![](youtube-embed:ID)` — is NOT raw HTML, isn't blocked by that flag,
// and produces the identical `<img src="youtube-embed:ID" alt="">` md4c
// would have produced from a real rewrite, which this function then
// substitutes exactly the same way. Confirmed live (2026-09-10 ASan
// adversarial pass, docs/architecture.md). This is harmless under the
// CURRENT trust model — the same single admin who can type either form
// can already embed any YouTube video ID by pasting a real URL, so typing
// the marker syntax directly grants no new capability — but it means this
// substitution is NOT gated on "came from a real URL rewrite" the way an
// earlier version of this comment claimed; it only checks the resulting
// HTML shape, regardless of provenance. Re-check this reasoning if this
// codebase ever adds multiple editors with different trust levels.
// This is the one and only place in this whole renderer where a
// URL-derived value reaches raw HTML output — `ID` has already been
// validated to exactly 11 URL-safe characters by rewriteYouTubeEmbeds
// before this function ever runs (when reached via that path), and is
// re-validated by the character scan right here regardless, the same
// "re-check even though it's supposed to already be safe" discipline
// this codebase applies at every other trust boundary (e.g. every MCP
// tool re-checking a resolved document's own visibility regardless of
// the caller's scope) — which is exactly what keeps this substitution
// safe even when reached via the direct-markdown-syntax path above:
// `ID` still can't be anything other than 11 URL-safe characters either
// way.
std::string substituteYouTubeEmbeds(std::string html) {
  constexpr std::string_view kMarkerOpen = "<img src=\"youtube-embed:";
  constexpr std::string_view kMarkerClose = "\" alt=\"\">";

  std::string out;
  out.reserve(html.size());
  size_t pos = 0;
  while (true) {
    const size_t open = html.find(kMarkerOpen, pos);
    if (open == std::string::npos) {
      out.append(html, pos, std::string::npos);
      break;
    }
    out.append(html, pos, open - pos);

    const size_t idStart = open + kMarkerOpen.size();
    bool ok = idStart + 11 <= html.size();
    for (size_t i = 0; ok && i < 11; ++i) ok = isYouTubeIdChar(html[idStart + i]);
    const size_t afterId = idStart + 11;
    ok = ok && afterId + kMarkerClose.size() <= html.size() &&
         html.compare(afterId, kMarkerClose.size(), kMarkerClose) == 0;

    if (!ok) {
      // Doesn't match the exact shape rewriteYouTubeEmbeds produces --
      // shouldn't happen (see this function's own doc comment), but
      // degrades to leaving the literal marker text alone rather than
      // guessing at a substitution.
      out.append(html, open, kMarkerOpen.size());
      pos = open + kMarkerOpen.size();
      continue;
    }

    const std::string videoId = html.substr(idStart, 11);
    out.append("<iframe class=\"youtube-embed\" src=\"https://www.youtube.com/embed/")
        .append(videoId)
        .append(
            "\" title=\"YouTube video player\" "
            "allow=\"accelerometer; autoplay; clipboard-write; encrypted-media; "
            "gyroscope; picture-in-picture; web-share\" "
            "referrerpolicy=\"strict-origin-when-cross-origin\" allowfullscreen "
            "loading=\"lazy\"></iframe>");
    pos = afterId + kMarkerClose.size();
  }
  return out;
}

// Swaps a fenced code block md4c rendered with the `mermaid` info-string
// (` ```mermaid ... ``` `) for the shape mermaid.js's default `pre.mermaid`
// selector actually looks for. md4c-html's render_open_code_block()
// (md4c-html.c) always emits exactly `<pre><code class="language-LANG">`
// for a fenced block with a known info string, HTML-escaping the code
// content itself — confirmed by reading that function directly, not
// guessed. No pre-parse rewrite is needed here the way YouTube embeds
// need one: unlike a bare URL, a fenced code block with an info string is
// already a first-class CommonMark/GFM construct md4c parses for free:
// this is a pure post-substitution, same technique as
// substituteYouTubeEmbeds above, on a completely disjoint HTML shape.
// The (already-escaped) content is copied through untouched — the
// browser decodes HTML entities via .textContent before mermaid.js ever
// reads it, so no unescaping belongs here. Requires an EXACT
// `class="language-mermaid"` match (nothing appended after `mermaid` in
// the fence's info string, e.g. not ` ```mermaid live `); anything else
// falls through unchanged and renders as an ordinary code block rather
// than guessing at a partial match.
std::string substituteMermaidBlocks(std::string html) {
  constexpr std::string_view kOpenMarker = "<pre><code class=\"language-mermaid\">";
  constexpr std::string_view kOpenReplacement = "<pre class=\"mermaid\">";
  constexpr std::string_view kCloseMarker = "</code></pre>";
  constexpr std::string_view kCloseReplacement = "</pre>";

  std::string out;
  out.reserve(html.size());
  size_t pos = 0;
  while (true) {
    const size_t open = html.find(kOpenMarker, pos);
    if (open == std::string::npos) {
      out.append(html, pos, std::string::npos);
      break;
    }
    out.append(html, pos, open - pos);

    const size_t contentStart = open + kOpenMarker.size();
    const size_t close = html.find(kCloseMarker, contentStart);
    if (close == std::string::npos) {
      // Shouldn't happen -- md4c always closes what it opens -- but
      // degrade to leaving the rest of the document untouched rather
      // than guessing at a malformed match.
      out.append(html, open, std::string::npos);
      break;
    }

    out.append(kOpenReplacement);
    out.append(html, contentStart, close - contentStart);
    out.append(kCloseReplacement);
    pos = close + kCloseMarker.size();
  }
  return out;
}

// Same technique as substituteMermaidBlocks immediately above, for
// ` ```query ... ``` ` blocks instead — see that function's own comment
// for why this needs no pre-parse rewrite and why the (already
// HTML-escaped) content is copied through untouched. The actual query
// execution happens server-side too, but NOT here: this function only
// ever produces a `<pre class="query">` placeholder holding the raw DSL
// text; static/js/query-block.js reads it client-side and calls
// GET /api/query (index::QueryBlocks::parseAndRun, src/controllers/
// QueryRoutes.cpp) to get live, visibility-gated results on every page
// view — MarkdownRenderer has no database handle at all (see
// docs/architecture.md's two-binary layout: wikicore's markdown
// rendering is deliberately DB-free), and a query embedded in a
// document must re-run on every view anyway, not freeze at save time,
// for it to be useful as a live index/dashboard rather than a snapshot.
std::string substituteQueryBlocks(std::string html) {
  constexpr std::string_view kOpenMarker = "<pre><code class=\"language-query\">";
  constexpr std::string_view kOpenReplacement = "<pre class=\"query\">";
  constexpr std::string_view kCloseMarker = "</code></pre>";
  constexpr std::string_view kCloseReplacement = "</pre>";

  std::string out;
  out.reserve(html.size());
  size_t pos = 0;
  while (true) {
    const size_t open = html.find(kOpenMarker, pos);
    if (open == std::string::npos) {
      out.append(html, pos, std::string::npos);
      break;
    }
    out.append(html, pos, open - pos);

    const size_t contentStart = open + kOpenMarker.size();
    const size_t close = html.find(kCloseMarker, contentStart);
    if (close == std::string::npos) {
      out.append(html, open, std::string::npos);
      break;
    }

    out.append(kOpenReplacement);
    out.append(html, contentStart, close - contentStart);
    out.append(kCloseReplacement);
    pos = close + kCloseMarker.size();
  }
  return out;
}

// Swaps a ```circuit fenced block for a view-only, non-interactive
// <iframe> running circuitjs1 (vendored at static/js/circuitjs1/ — see
// static/js/circuitjs1/VENDORED.md for provenance and the one source
// patch applied) against the block's own content as the circuit to load.
// Same post-substitution technique as substituteMermaidBlocks/
// substituteQueryBlocks above — md4c already parsed the fence into
// `<pre><code class="language-circuit">RAW XML, HTML-escaped</code></pre>`
// for free, and that escaped content is copied straight into the
// data-circuit-xml attribute below, untouched — the exact same reasoning
// as those two functions' own comments: md4c-html's render_html_escaped
// only ever escapes & < > ", the identical set and identical replacement
// strings this renderer's own escapeHtml uses for HTML ATTRIBUTE content,
// so what's already between the <pre><code> markers is already valid to
// drop straight into any other HTML attribute, no unescape/re-escape
// round-trip needed.
//
// The circuit is loaded via the `CircuitJS1.importCircuit()` same-origin
// JS API (static/js/circuit-embed.js reads data-circuit-xml back off
// this element and calls it once the iframe's own oncircuitjsloaded
// fires), NOT via a `cct=` query parameter the way an earlier version of
// this function worked — circuitjs1's own query-string decoder
// (QueryParameters.java, GWT's URL.decode(), which matches JS decodeURI
// not decodeURIComponent) never decodes a handful of URI-reserved
// characters including `=` and `/`, both of which this XML format's own
// attribute syntax (`attr="value"`) and self-closing tags (`/>`) use
// constantly — there is no percent-encoding of the value that survives
// that specific decoder's own round trip once the XML actually has
// attributes, confirmed live (a real circuit with `<r ... r="20"/>`
// reached the iframe as `r%3D"20"` — the literal three characters,
// `%`/`3`/`D` — and failed to parse as XML at all). The hideSidebar/
// hideMenu/hideInfoBox/editable/running flags below stay on the URL
// since they're plain ASCII with no such characters, unaffected by that
// decoder gap.
std::string substituteCircuitBlocks(std::string html) {
  constexpr std::string_view kOpenMarker = "<pre><code class=\"language-circuit\">";
  constexpr std::string_view kCloseMarker = "</code></pre>";
  // Relative, no leading slash: resolved against shell.html's own
  // dynamically-injected <base> tag, same reasoning as mermaid-render.js's
  // own script.src assignment (see that file's comment) — a leading
  // slash or the document's own path has nothing to do with where this
  // app's static files actually live once routed under a URL prefix.
  constexpr std::string_view kViewerUrl =
      "js/circuitjs1/circuitjs.html?hideSidebar=true&amp;hideMenu=true"
      "&amp;hideInfoBox=true&amp;editable=false&amp;running=true";

  std::string out;
  out.reserve(html.size());
  size_t pos = 0;
  while (true) {
    const size_t open = html.find(kOpenMarker, pos);
    if (open == std::string::npos) {
      out.append(html, pos, std::string::npos);
      break;
    }
    out.append(html, pos, open - pos);

    const size_t contentStart = open + kOpenMarker.size();
    const size_t close = html.find(kCloseMarker, contentStart);
    if (close == std::string::npos) {
      out.append(html, open, std::string::npos);
      break;
    }

    out.append("<iframe class=\"circuit-embed\" src=\"")
        .append(kViewerUrl)
        .append("\" data-circuit-xml=\"")
        .append(html, contentStart, close - contentStart)
        .append("\" loading=\"lazy\"></iframe>");
    pos = close + kCloseMarker.size();
  }
  return out;
}

}  // namespace

std::string renderMarkdownToHtml(std::string_view markdown) {
  const std::string preprocessed = rewriteYouTubeEmbeds(markdown);

  std::string html;

  constexpr unsigned kParserFlags =
      MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
      MD_FLAG_PERMISSIVEAUTOLINKS |
      // Raw HTML passthrough is off on purpose: a document written while
      // private shouldn't get to inject arbitrary HTML/JS just because it
      // later gets flipped to public. There's no separate sanitizer in
      // front of this renderer's output — this flag IS the sanitization.
      MD_FLAG_NOHTMLBLOCKS | MD_FLAG_NOHTMLSPANS;
  constexpr unsigned kRendererFlags = 0;

  const int rc =
      md_html(preprocessed.data(), static_cast<MD_SIZE>(preprocessed.size()),
              &appendOutput, &html, kParserFlags, kRendererFlags);
  if (rc != 0) {
    throw std::runtime_error("markdown rendering failed");
  }
  return substituteCircuitBlocks(substituteQueryBlocks(
      substituteMermaidBlocks(substituteYouTubeEmbeds(std::move(html)))));
}

}  // namespace wikicore::util
