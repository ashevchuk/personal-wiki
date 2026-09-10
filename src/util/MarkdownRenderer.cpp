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
  return substituteYouTubeEmbeds(std::move(html));
}

}  // namespace wikicore::util
