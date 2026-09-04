#pragma once

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

// Registers every URL a human navigates to directly (/, /login, /search,
// /folder[/...], /d/{path...}, /edit/{path...}, /account) — each one returns the
// EXACT SAME static file (static/shell.html) verbatim, regardless of the
// specific path or any request data. This is the standard client-rendered
// app fallback: the browser's own JS (static/js/router.js) inspects
// location.pathname and renders the actual page from JSON fetched off the
// /api/* routes. Nothing here builds HTML from a request — see
// docs/architecture.md's frontend section for why that split exists.
//
// Drogon's own static-file serving (setDocumentRoot) can't cover these:
// it only serves a request whose URI literally matches a file under
// static/, so /d/notes/foo.md would 404 without an explicit handler
// mapping it back to the shell.
void registerPageRoutes(drogon::HttpAppFramework& app);

}  // namespace wikicore::controllers
