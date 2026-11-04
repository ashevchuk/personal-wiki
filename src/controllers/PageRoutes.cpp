#include "controllers/PageRoutes.h"

#include <drogon/HttpResponse.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace drogon;

namespace wikicore::controllers {

namespace {

struct ThemeInfo {
  std::string name;    // CSS file stem, e.g. "green" -- also the data-theme value
  std::string label;    // Menu button text, e.g. "Green"
  std::string foucBg;   // Pre-stylesheet-load placeholder background
  std::string foucFg;   // Pre-stylesheet-load placeholder text color
};

// Pulls a `key: value` out of a `/* ... */` metadata comment near the top
// of a theme CSS file (e.g. "fouc-bg: #000;"), stopping at the first `;`
// or `*`. Not a general CSS parser -- just enough to read the one or two
// keys each theme file declares for itself; see any themes/*.css's own
// comment on why these can't be read from the theme's real --bg/--fg
// custom properties instead (the stylesheet hasn't loaded yet at the
// point shell.html needs this).
std::string extractMetaValue(const std::string& css, const std::string& key) {
  auto pos = css.find(key);
  if (pos == std::string::npos) return {};
  pos += key.size();
  auto end = css.find_first_of(";*", pos);
  if (end == std::string::npos) return {};
  auto start = css.find_first_not_of(" \t", pos);
  if (start == std::string::npos || start >= end) return {};
  auto lastNonSpace = css.find_last_not_of(" \t", end - 1);
  return css.substr(start, lastNonSpace - start + 1);
}

// Scans static/css/themes/*.css so shell.html's theme list, anti-FOUC
// placeholder colors, and theme-menu buttons all come from whatever
// theme files actually exist on disk, instead of being hand-maintained
// in three separate places (shell.html twice, theme.js once) in sync
// with each other.
std::vector<ThemeInfo> discoverThemes() {
  std::vector<ThemeInfo> themes;
  namespace fs = std::filesystem;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator("static/css/themes", ec)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".css") continue;

    std::ifstream file(entry.path(), std::ios::binary);
    std::ostringstream buf;
    buf << file.rdbuf();
    const std::string css = buf.str();

    ThemeInfo info;
    info.name = entry.path().stem().string();
    info.label = info.name;
    if (!info.label.empty()) {
      info.label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(info.label[0])));
    }
    info.foucBg = extractMetaValue(css, "fouc-bg:");
    info.foucFg = extractMetaValue(css, "fouc-fg:");
    if (info.foucBg.empty() || info.foucFg.empty()) {
      LOG_ERROR << "theme '" << info.name << "' (static/css/themes/" << info.name
                << ".css) has no fouc-bg/fouc-fg metadata comment -- it will "
                   "flash unstyled before its stylesheet loads";
    }
    themes.push_back(std::move(info));
  }
  if (ec) {
    LOG_ERROR << "could not scan static/css/themes: " << ec.message();
  }
  std::sort(themes.begin(), themes.end(),
            [](const ThemeInfo& a, const ThemeInfo& b) { return a.name < b.name; });
  return themes;
}

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

  const std::vector<ThemeInfo> themes = discoverThemes();

  {
    // Lands in the SAME injected <script>, first thing inside <head>,
    // before shell.html's own inline bootstrap scripts — see those
    // scripts' own comments for why each one takes priority over the
    // client-side fallback it has for the same decision (basePath over
    // location.pathname pattern-matching; defaultTheme over the
    // hardcoded "green" — though defaultTheme is always second-priority
    // to whatever the reader already picked via localStorage, unlike
    // basePath which has no such per-reader override). __WIKI_THEMES__
    // is injected unconditionally (unlike the other two, which only
    // exist when their config key is set) since it's not optional admin
    // config -- it's the list of theme files that actually exist on disk.
    const std::string marker = "<head>";
    const auto pos = g_shellHtml.find(marker);
    if (pos != std::string::npos) {
      std::string injected = marker + "\n<script>window.__WIKI_THEMES__=[";
      for (size_t i = 0; i < themes.size(); ++i) {
        if (i > 0) injected += ",";
        injected += "\"" + escapeForJsString(themes[i].name) + "\"";
      }
      injected += "];";
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
      LOG_ERROR << "static/shell.html has no literal \"<head>\" to inject "
                   "into -- theme list, base_path and/or default theme "
                   "will have no effect until this is fixed";
    }
  }

  {
    const std::string marker = "/* THEME_FOUC_RULES */";
    const auto pos = g_shellHtml.find(marker);
    if (pos != std::string::npos) {
      std::string rules;
      for (const auto& t : themes) {
        if (t.foucBg.empty() || t.foucFg.empty()) continue;
        rules += "html[data-theme=\"" + t.name + "\"]:not(.is-ready),";
        rules += "html[data-theme=\"" + t.name + "\"]:not(.is-ready) body { background: " +
                  t.foucBg + "; color: " + t.foucFg + "; }\n";
      }
      g_shellHtml.replace(pos, marker.size(), rules);
    } else {
      LOG_ERROR << "static/shell.html has no \"" << marker
                << "\" marker -- pages will flash unstyled before their "
                   "theme stylesheet loads";
    }
  }

  {
    const std::string marker = "<!--THEME_MENU_BUTTONS-->";
    const auto pos = g_shellHtml.find(marker);
    if (pos != std::string::npos) {
      std::string buttons;
      for (const auto& t : themes) {
        buttons += "<button type=\"button\" data-theme=\"" + t.name + "\">" + t.label + "</button>";
      }
      g_shellHtml.replace(pos, marker.size(), buttons);
    } else {
      LOG_ERROR << "static/shell.html has no \"" << marker
                << "\" marker -- the theme picker menu will be empty";
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
  registerShellRoute(app, "/calendar");
}

}  // namespace wikicore::controllers
