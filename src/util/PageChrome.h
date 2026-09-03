#pragma once

#include <string>

namespace wikicore::util {

// Wraps `bodyContent` (already-safe HTML — the caller is responsible for
// escaping anything untrusted before it reaches this function, same as
// everywhere else in this codebase) in the shared page shell used by
// every page: the green-on-black theme (theme.css), the Matrix-rain
// canvas background (matrix.js), and the sidebar (nav.js populates
// #nav-tree/#nav-tags from the existing /api/nav/tree and /api/nav/tags
// endpoints — see NavRoutes.cpp).
//
// Used directly by the two hand-built-HTML routes (AuthRoutes.cpp's login
// page, DocumentRoutes.cpp's document view) — the CSP-templated pages
// (EditPage.csp, SearchPage.csp) can't call an arbitrary C++ function
// inline as naturally as writing the same static markup by hand with
// [[basePath]] substitutions, so they duplicate this shell's structure
// directly in the .csp file instead. Keep the two in sync if this
// changes.
//
// `escapedTitle` must already be HTML-escaped by the caller (same
// discipline as everywhere else — see util::escapeHtml).
std::string renderPage(const std::string& basePath, const std::string& escapedTitle,
                        const std::string& bodyContent);

// "Home / notes / sub / foo.md" from a vault-relative path (a document's
// own path, or a folder's). Folder segments are plain text, not links:
// there's no per-folder "browse this exact ancestor" shortcut needed
// beyond what the full /folder/{path} page itself already is — only
// "Home" links anywhere, the trailing segment is styled as the current
// page/folder. `path` may be empty (renders just "Home").
std::string renderBreadcrumbs(const std::string& basePath, const std::string& path);

}  // namespace wikicore::util
