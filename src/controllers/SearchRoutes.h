#pragma once

#include "index/FtsSearch.h"

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

// GET /api/search?q=&tag=&type= — JSON array of matching documents,
// visibility-gated by the caller's auth state. No HTML here — see
// docs/architecture.md's frontend section; static/js/pages/search.js
// renders the results (including the snippet's <mark> highlighting,
// mirroring the same escape-then-substitute order this used to do
// server-side — see the `snippet`/`snippetIsHighlighted` fields below).
//
// GET /search itself (the page a human navigates to) is handled by
// PageRoutes.cpp, not here — it just serves the static SPA shell.
void registerSearchRoutes(drogon::HttpAppFramework& app, wikicore::index::FtsSearch& search);

}  // namespace wikicore::controllers
