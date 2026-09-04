#include "controllers/PageRoutes.h"

#include <drogon/HttpResponse.h>
#include <trantor/utils/Logger.h>

#include <fstream>
#include <sstream>

using namespace drogon;

namespace wikicore::controllers {

namespace {

// Cached across every request — built once from registerPageRoutes(),
// read-only from then on (every request-handling thread only ever calls
// shellResponse(), never writes these), so no synchronization needed.
// Rebuilding the file read + string insert on every single page load for
// content that can't change without a process restart would be pure waste.
std::string g_shellHtml;
bool g_shellHtmlReady = false;

// Minimal JS string-literal escaping for the one value that ever lands
// inside <script> text here: cfg.basePath, admin-supplied from
// config.toml. Low risk (it's a trusted local file, not user input) but
// escaped anyway rather than assumed clean — a stray `"` in a typo'd
// base_path should produce a harmless wrong prefix, not a syntax error
// that breaks the ENTIRE page for every visitor.
std::string escapeForJsString(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\\' || c == '"') out.push_back('\\');
    if (c == '<') {
      // Defuse "</script>" appearing inside the string literal, which
      // would otherwise close the tag early regardless of JS quoting.
      out += "\\u003c";
      continue;
    }
    out.push_back(c);
  }
  return out;
}

void buildShellHtml(const std::string& basePath) {
  std::ifstream file("static/shell.html", std::ios::binary);
  std::ostringstream buf;
  buf << file.rdbuf();
  g_shellHtml = buf.str();

  if (!basePath.empty()) {
    // Injected as the very first thing inside <head>, before shell.html's
    // own inline bootstrap script — see that script's own comment for why
    // an authoritative, server-confirmed prefix takes priority over its
    // location.pathname pattern-matching (which can't ever recover a
    // prefix for a path matching no known route, e.g. a stale
    // [[wiki-link]], on a browser with nothing cached yet either — see
    // AppConfig.h's own comment on why base_path was reintroduced).
    const std::string marker = "<head>";
    const auto pos = g_shellHtml.find(marker);
    if (pos != std::string::npos) {
      const std::string injected =
          marker + "\n<script>window.__WIKI_KNOWN_BASE_PATH__=\"" +
          escapeForJsString(basePath) + "\";</script>";
      g_shellHtml.replace(pos, marker.size(), injected);
    } else {
      // static/shell.html got restructured without a literal "<head>" —
      // fail loud rather than silently shipping every page without the
      // configured base_path (which would look like this setting doing
      // nothing, a much harder bug to track down than a startup log line).
      LOG_ERROR << "base_path is configured but static/shell.html has no "
                   "literal \"<head>\" to inject into — base_path will "
                   "have no effect until this is fixed";
    }
  }
  g_shellHtmlReady = true;
}

}  // namespace

HttpResponsePtr shellResponse() {
  // A fresh HttpResponsePtr per request (no caching the HttpResponsePtr
  // itself across requests/threads) — only the underlying HTML string is
  // shared; response objects carry their own per-request state (e.g.
  // Drogon's internal send buffers) that isn't safe to share.
  auto resp = HttpResponse::newHttpResponse();
  resp->setStatusCode(k200OK);
  resp->setContentTypeCode(CT_TEXT_HTML);
  resp->setBody(g_shellHtmlReady ? g_shellHtml : std::string());
  return resp;
}

namespace {

void registerShellRoute(HttpAppFramework& app, const std::string& path) {
  app.registerHandler(
      path,
      [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback) {
        callback(shellResponse());
      },
      {Get});
}

void registerShellRoutePrefix(HttpAppFramework& app, const std::string& regex) {
  app.registerHandlerViaRegex(
      regex,
      [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string&) { callback(shellResponse()); },
      {Get});
}

}  // namespace

void registerPageRoutes(HttpAppFramework& app, const wikicore::config::AppConfig& cfg) {
  buildShellHtml(cfg.basePath);

  registerShellRoute(app, "/");
  registerShellRoute(app, "/login");
  registerShellRoute(app, "/search");
  registerShellRoute(app, "/folder");
  registerShellRoutePrefix(app, "^/folder/(.*)$");
  registerShellRoutePrefix(app, "^/d/(.*)$");
  registerShellRoutePrefix(app, "^/edit/(.*)$");
  registerShellRoutePrefix(app, "^/history/(.*)$");
  registerShellRoute(app, "/account");
}

}  // namespace wikicore::controllers
