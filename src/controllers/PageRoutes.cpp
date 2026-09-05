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

// Minimal JS string-literal escaping for the values that land inside
// <script> text here: cfg.basePath and cfg.defaultTheme, both
// admin-supplied from config.toml. Low risk (it's a trusted local file,
// not user input) but escaped anyway rather than assumed clean — a
// stray `"` in a typo'd value should produce a harmless wrong
// prefix/theme name, not a syntax error that breaks the ENTIRE page
// for every visitor.
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

void buildShellHtml(const std::string& basePath, const std::string& defaultTheme) {
  std::ifstream file("static/shell.html", std::ios::binary);
  std::ostringstream buf;
  buf << file.rdbuf();
  g_shellHtml = buf.str();

  if (!basePath.empty() || !defaultTheme.empty()) {
    // Both land in the SAME injected <script>, first thing inside <head>,
    // before shell.html's own inline bootstrap scripts — see those
    // scripts' own comments for why each one takes priority over the
    // client-side fallback it has for the same decision (basePath over
    // location.pathname pattern-matching; defaultTheme over the
    // hardcoded "green" — though defaultTheme is always second-priority
    // to whatever the reader already picked via localStorage, unlike
    // basePath which has no such per-reader override).
    const std::string marker = "<head>";
    const auto pos = g_shellHtml.find(marker);
    if (pos != std::string::npos) {
      std::string injected = marker + "\n<script>";
      if (!basePath.empty()) {
        injected += "window.__WIKI_KNOWN_BASE_PATH__=\"" +
                     escapeForJsString(basePath) + "\";";
      }
      if (!defaultTheme.empty()) {
        injected += "window.__WIKI_DEFAULT_THEME__=\"" +
                     escapeForJsString(defaultTheme) + "\";";
      }
      injected += "</script>";
      g_shellHtml.replace(pos, marker.size(), injected);
    } else {
      // static/shell.html got restructured without a literal "<head>" —
      // fail loud rather than silently shipping every page without
      // whichever of these settings is configured (which would look like
      // the setting doing nothing, a much harder bug to track down than
      // a startup log line).
      LOG_ERROR << "base_path and/or theme is configured but "
                   "static/shell.html has no literal \"<head>\" to inject "
                   "into — that setting will have no effect until this is "
                   "fixed";
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
  buildShellHtml(cfg.basePath, cfg.defaultTheme);

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
