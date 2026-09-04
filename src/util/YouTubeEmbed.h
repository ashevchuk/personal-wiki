#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace wikicore::util {

// Extracts an 11-character YouTube video ID from any of the URL shapes
// people actually paste: youtube.com/watch?v=ID (v= anywhere in the
// query string, other params on either side), youtu.be/ID,
// youtube.com/shorts/ID, youtube.com/embed/ID — each with or without a
// leading "www."/"m.", http or https, and an optional trailing query
// string (Shorts' own share links append "?feature=share"). Returns
// nullopt for anything else, including a URL that merely resembles one
// of these (wrong host, an ID that isn't exactly 11 URL-safe
// characters) — this is the one and only gate before a URL-derived
// value is allowed anywhere near MarkdownRenderer's iframe
// substitution, so it fails closed rather than guessing.
std::optional<std::string> extractYouTubeVideoId(std::string_view url);

// Pre-processing pass over a document's raw markdown, mirroring
// WikiLinks::rewriteWikiLinksToMarkdownLinks in shape but not in where
// it's called from — see MarkdownRenderer.cpp for why this one is
// invoked FROM renderMarkdownToHtml itself rather than externally at the
// DocumentRoutes.cpp call site. Rewrites every "![youtube](URL)" (the
// alt text "youtube", case-insensitive, is the sentinel that opts an
// image into embed handling — anything else stays a normal image) whose
// URL survives extractYouTubeVideoId above into "![](youtube-embed:ID)",
// a URL scheme that will never collide with a real image URL and that
// ordinary CommonMark image parsing (NOT raw HTML passthrough — md4c has
// no idea any of this exists) turns into a plain, inert
// `<img src="youtube-embed:ID" alt="">`. Left completely unchanged if
// the URL isn't recognized — falls through to a normal, harmlessly
// broken `<img>` render rather than silently dropping the author's text.
std::string rewriteYouTubeEmbeds(std::string_view markdown);

}  // namespace wikicore::util
