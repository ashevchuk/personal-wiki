#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace wikicore::util {

// `[[notes/foo]]` / `[[notes/foo|Custom label]]` — Obsidian/MediaWiki-style
// wiki-links, layered on top of md4c (which has no idea this syntax
// exists — see MarkdownRenderer.h) as a small preprocessing pass instead
// of a custom parser extension. Target is always normalized the same way
// in both functions below, so a link extracted by extractWikiLinkTargets
// and a document's own vault-relative `path` compare equal directly, with
// no further munging needed at either the write (IndexUpdater) or read
// (NavQueries::backlinks) side:
//   - leading/trailing whitespace trimmed
//   - a leading '/' stripped (this app's paths never start with one)
//   - ".md" appended if the target doesn't already end with it (lets
//     `[[notes/foo]]` and `[[notes/foo.md]]` both mean the same document,
//     matching the common Obsidian-style convention of omitting it)

// Every wiki-link TARGET in `markdown`, normalized and de-duplicated
// (insertion order). Used by IndexUpdater to populate `document_links` —
// a target that doesn't correspond to any real document yet is still
// recorded as-is (a "red link"): the moment a document at that path gets
// created, the backlink becomes valid with no re-edit of the linking
// document required, since nothing here depends on the target existing.
std::vector<std::string> extractWikiLinkTargets(std::string_view markdown);

// Rewrites every `[[target]]` / `[[target|label]]` into a plain
// `[label](d/target)` CommonMark link (label defaults to the target text,
// exactly as written before normalization, when no `|label` is given) —
// anything md4c already knows how to render. The href is `d/` + the
// *normalized* target, always relative (no leading '/'), so it resolves
// correctly against this app's own <base href="{basePath}/"> tag (see
// shell.html) the same way every other relative link/asset URL in this
// app does, independent of any reverse-proxy subpath the app is mounted under. The
// `d/` prefix is NOT optional — normalizeTarget()'s output is a path in
// FILE space (matching the vault's own directory structure), but viewing
// a document is a ROUTE at `/d/{path}`, not the bare path itself; an
// earlier version of this function emitted the bare target and shipped
// broken links to production before a real user caught it by clicking
// one (every test at the time asserted the bare, broken shape as
// "correct" — they'd been written by copying what the code produced, not
// by independently deriving what the URL actually needed to be). Called
// once per render, right before renderMarkdownToHtml — MarkdownRenderer
// itself stays unaware this syntax exists at all.
std::string rewriteWikiLinksToMarkdownLinks(std::string_view markdown);

// Rewrites every `[[target]]` / `[[target|label]]` whose *normalized*
// target equals `oldNormalizedPath` so it points at `newNormalizedPath`
// instead. Used by DocumentService::rename to keep inbound wiki-links
// valid after a document moves. Author conventions are preserved:
// `[[notes/foo]]` (no ".md") becomes `[[notes/bar]]`, `[[notes/foo.md]]`
// becomes `[[notes/bar.md]]`, and a `|label` is left untouched. Targets
// that normalize to anything else pass through verbatim, including the
// surrounding markdown. A no-op (and returns a copy of `markdown`) when
// the two paths are equal or either is empty.
std::string rewriteWikiLinkTargets(std::string_view markdown,
                                   std::string_view oldNormalizedPath,
                                   std::string_view newNormalizedPath);

// Same author-convention preservation as rewriteWikiLinkTargets, but
// every normalized target that *starts with* `oldPrefix` (a folder path
// with a trailing '/', e.g. "notes/cpp/") has that prefix replaced by
// `newPrefix`. Used by FolderService::move so inbound `[[wiki-link]]`s
// into a relocated subtree stay valid. A target that is exactly the
// folder name plus ".md" (`[[notes/cpp]]` → notes/cpp.md) is NOT a
// child of the folder and is left alone. No-op when the prefixes are
// equal or either is empty.
std::string rewriteWikiLinkTargetPrefix(std::string_view markdown,
                                        std::string_view oldPrefix,
                                        std::string_view newPrefix);

}  // namespace wikicore::util
