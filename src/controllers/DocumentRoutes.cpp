#include "controllers/DocumentRoutes.h"

#include "auth/AuthContext.h"
#include "util/MarkdownRenderer.h"
#include "vault/FrontMatter.h"
#include "vault/PathGuard.h"

#include <drogon/HttpResponse.h>

#include <filesystem>
#include <optional>

using namespace drogon;
using namespace wikicore::auth;
using namespace wikicore::vault;

namespace wikicore::controllers {

namespace {

HttpResponsePtr notFound() {
  auto resp = HttpResponse::newHttpResponse();
  resp->setStatusCode(k404NotFound);
  resp->setContentTypeCode(CT_TEXT_PLAIN);
  resp->setBody("not found\n");
  return resp;
}

HttpResponsePtr renderDocument(const FrontMatter& fm, const std::string& body) {
  const std::string title = fm.title.empty() ? "(untitled)" : fm.title;
  std::string escapedTitle;
  for (char c : title) {
    if (c == '<') escapedTitle += "&lt;";
    else if (c == '>') escapedTitle += "&gt;";
    else if (c == '&') escapedTitle += "&amp;";
    else escapedTitle += c;
  }

  auto resp = HttpResponse::newHttpResponse();
  resp->setContentTypeCode(CT_TEXT_HTML);
  resp->setBody("<!doctype html><html><head><meta charset=\"utf-8\"><title>" +
                escapedTitle + "</title></head><body><h1>" + escapedTitle +
                "</h1>" + util::renderMarkdownToHtml(body) + "</body></html>");
  return resp;
}

}  // namespace

void registerDocumentRoutes(HttpAppFramework& app, VaultRepository& vault) {
  app.registerHandlerViaRegex(
      "^/d/(.*)$",
      [&vault](const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)>&& callback,
               const std::string& docPath) {
        std::string raw;
        try {
          raw = vault.readRaw(docPath);
        } catch (const PathTraversalError&) {
          callback(notFound());
          return;
        } catch (const std::filesystem::filesystem_error&) {
          callback(notFound());
          return;
        }

        const ParsedDocument parsed = parseFrontMatter(raw);

        const bool authenticated =
            req->attributes()->get<std::optional<int64_t>>(kAttrUserId)
                .has_value();
        if (parsed.frontMatter.visibility != "public" && !authenticated) {
          // Fail-safe-private: malformed/missing visibility already
          // defaults to "private" inside parseFrontMatter, so this one
          // comparison covers both "explicitly private" and "unparseable".
          callback(notFound());
          return;
        }

        callback(renderDocument(parsed.frontMatter, parsed.body));
      },
      // DrClassMap registers filters under their fully-qualified,
      // demangled type name (__cxa_demangle), not the bare class name.
      {Get, "wikicore::auth::AuthFilter"});
}

}  // namespace wikicore::controllers
