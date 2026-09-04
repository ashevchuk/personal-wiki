#pragma once

#include <drogon/HttpAppFramework.h>

#include "config/AppConfig.h"

namespace wikicore::controllers {

// Registers every URL a human navigates to directly (/, /login, /search,
// /folder[/...], /d/{path...}, /edit/{path...}, /account) — each one returns the
// EXACT SAME rendered shell (static/shell.html, verbatim unless
// cfg.basePath is set — see shellResponse() below), regardless of the
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
//
// Also caches the rendered shell body (see shellResponse()) keyed off
// cfg.basePath — call this once, before app().run(), same as every other
// route registration.
void registerPageRoutes(drogon::HttpAppFramework& app, const wikicore::config::AppConfig& cfg);

// Builds (on first call) and returns the same shell response every route
// above serves. Exposed separately so main.cpp's setDefaultHandler can
// reuse the EXACT same body (base_path injection included) for a request
// that matches no route at all — it only needs to override the status
// code to 404 afterward. registerPageRoutes() must run first so the
// cache reflects the real cfg.basePath instead of the empty default.
drogon::HttpResponsePtr shellResponse();

}  // namespace wikicore::controllers
