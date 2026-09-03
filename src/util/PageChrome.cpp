#include "util/PageChrome.h"

#include "util/BasePath.h"
#include "util/HtmlEscape.h"

namespace wikicore::util {

std::string renderPage(const std::string& basePath, const std::string& escapedTitle,
                        const std::string& bodyContent) {
  return
      "<!doctype html><html><head><meta charset=\"utf-8\">"
      "<meta name=\"wiki-base-path\" content=\"" + escapeHtml(basePath) + "\">"
      "<title>" + escapedTitle + "</title>"
      "<link rel=\"stylesheet\" href=\"" + withBasePath(basePath, "/css/theme.css") + "\">"
      "</head><body>"
      "<canvas id=\"matrix-bg\"></canvas>"
      "<div class=\"layout\">"
      "<nav class=\"sidebar\">"
      "<a class=\"sidebar-brand\" href=\"" + withBasePath(basePath, "/") + "\">wiki</a>"
      "<div class=\"sidebar-links\">"
      "<a href=\"" + withBasePath(basePath, "/search") + "\">Search</a>"
      " <a href=\"" + withBasePath(basePath, "/edit/untitled.md") + "\">+ New</a>"
      "</div>"
      "<div class=\"sidebar-section\"><h3>Tags</h3><div id=\"nav-tags\"></div></div>"
      "<div class=\"sidebar-section\"><h3>Documents</h3><div id=\"nav-tree\"></div></div>"
      "</nav>"
      "<main class=\"content\">" + bodyContent + "</main>"
      "</div>"
      "<script src=\"" + withBasePath(basePath, "/js/matrix.js") + "\"></script>"
      "<script src=\"" + withBasePath(basePath, "/js/nav.js") + "\"></script>"
      "</body></html>";
}

}  // namespace wikicore::util
