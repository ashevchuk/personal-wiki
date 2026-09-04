#pragma once

#include <string>
#include <string_view>

namespace wikicore::util {

// Renders a document body (markdown, GFM tables/strikethrough/tasklists
// enabled) to HTML. Raw HTML passthrough is deliberately disabled — see
// the .cpp — so this is safe to embed directly into a page without a
// separate sanitization pass. Throws std::runtime_error on (rare) parser
// failure.
//
// Also handles YouTube embeds (`![youtube](url)` — see util/YouTubeEmbed.h
// for the recognized URL shapes and the ID-extraction contract) as part
// of this same call, unlike wiki-links (`[[target]]`, WikiLinks.h), which
// stay an external pre-pass the CALLER chains in before this function.
// The difference isn't arbitrary: a wiki-link needs nothing from this
// renderer at all — it rewrites straight to a plain CommonMark
// `[label](target)` link that md4c already knows how to render, so
// keeping MarkdownRenderer "unaware" of that syntax costs nothing. A
// YouTube embed can't be expressed as any CommonMark construct md4c
// natively emits as-is — turning `<img>` into `<iframe>` requires
// touching md4c's own HTML output, which only this function ever sees —
// so splitting the embed feature's two halves (markdown-level rewrite,
// HTML-level substitution) across two separate call sites would only
// make it easier for one half to drift out of sync with the other.
std::string renderMarkdownToHtml(std::string_view markdown);

}  // namespace wikicore::util
