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

// Swaps every `<img src="youtube-embed:ID" alt="">` md4c produced (from
// the `![](youtube-embed:ID)` marker YouTubeEmbed::rewriteYouTubeEmbeds
// writes — the ONLY place that exact marker string can originate; a raw
// `<img ...>` typed by hand in a document body gets HTML-escaped to
// `&lt;img ...&gt;` by md4c same as any other literal HTML, since
// MD_FLAG_NOHTMLSPANS is on below, so this substitution can't be forged
// from a document's own text) for a real, narrowly-templated `<iframe>`.
// This is the one and only place in this whole renderer where a
// URL-derived value reaches raw HTML output — `ID` has already been
// validated to exactly 11 URL-safe characters by rewriteYouTubeEmbeds
// before this function ever runs, and is re-validated by the character
// scan right here regardless, the same "re-check even though it's
// supposed to already be safe" discipline this codebase applies at every
// other trust boundary (e.g. every MCP tool re-checking a resolved
// document's own visibility regardless of the caller's scope).
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
