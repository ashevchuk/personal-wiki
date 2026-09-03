#include "controllers/DocumentRoutes.h"

#include "auth/AuthContext.h"
#include "auth/RequireAdmin.h"
#include "util/BasePath.h"
#include "util/HtmlEscape.h"
#include "util/MarkdownRenderer.h"
#include "util/PageChrome.h"
#include "vault/FrontMatter.h"
#include "vault/PathGuard.h"

#include <drogon/HttpResponse.h>
#include <drogon/HttpViewData.h>
#include <drogon/MultiPart.h>

#include <nlohmann/json.hpp>

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

HttpResponsePtr jsonError(HttpStatusCode status, const std::string& message) {
  Json::Value body;
  body["error"] = message;
  auto resp = HttpResponse::newHttpJsonResponse(body);
  resp->setStatusCode(status);
  return resp;
}

HttpResponsePtr jsonOk(const std::string& path) {
  Json::Value body;
  body["path"] = path;
  return HttpResponse::newHttpJsonResponse(body);
}

// Renders a document view. When `authenticated`, adds an Edit link and a
// logout form (its hidden csrf field comes straight from what AuthFilter
// already put in req->attributes() — see kAttrCsrfToken).
HttpResponsePtr renderDocument(const std::string& basePath, const std::string& docPath,
                                const FrontMatter& fm, const std::string& body,
                                bool authenticated, const std::string& csrfToken) {
  const std::string title = fm.title.empty() ? "(untitled)" : fm.title;
  const std::string escapedTitle = util::escapeHtml(title);

  std::string chrome;
  if (authenticated) {
    chrome =
        "<p><a href=\"" + util::withBasePath(basePath, "/edit/") +
        util::escapeHtml(docPath) + "\">Edit</a> | "
        "<form style=\"display:inline\" method=\"post\" action=\"" +
        util::withBasePath(basePath, "/logout") + "\">"
        "<input type=\"hidden\" name=\"csrf_token\" value=\"" +
        util::escapeHtml(csrfToken) +
        "\"><button type=\"submit\">Log out</button></form></p>";
  }

  const std::string pageBody =
      chrome + "<h1>" + escapedTitle + "</h1>" + util::renderMarkdownToHtml(body);

  auto resp = HttpResponse::newHttpResponse();
  resp->setContentTypeCode(CT_TEXT_HTML);
  resp->setBody(util::renderPage(basePath, escapedTitle, pageBody));
  return resp;
}

// "notes/foo.assets/diagram.png" -> "notes/foo.md" (the owning document),
// or nullopt if `assetPath` isn't shaped like a co-located attachment path
// at all.
std::optional<std::string> owningDocumentFor(const std::string& assetPath) {
  const std::filesystem::path p(assetPath);
  const std::filesystem::path dir = p.parent_path();
  const std::string dirName = dir.filename().string();
  constexpr std::string_view kSuffix = ".assets";
  if (dirName.size() <= kSuffix.size() ||
      dirName.compare(dirName.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
    return std::nullopt;
  }
  const std::string stem = dirName.substr(0, dirName.size() - kSuffix.size());
  return (dir.parent_path() / (stem + ".md")).generic_string();
}

// Builds a DocumentInput from a parsed JSON request body. Throws
// std::invalid_argument (caught by the caller) on missing/malformed
// required fields.
DocumentInput parseDocumentInput(const Json::Value& json) {
  DocumentInput input;
  input.title = json.get("title", "").asString();
  input.type = json.get("type", "").asString();
  input.visibility = json.get("visibility", "private").asString();
  input.body = json.get("body", "").asString();
  if (json.isMember("tags") && json["tags"].isArray()) {
    for (const auto& tag : json["tags"]) {
      if (tag.isString()) input.tags.push_back(tag.asString());
    }
  }
  return input;
}

}  // namespace

void registerDocumentRoutes(HttpAppFramework& app, VaultRepository& vault,
                             DocumentService& documentService,
                             AttachmentService& attachmentService,
                             const std::string& basePath) {
  // --- GET /d/{path...} — view (M1, now with Edit/Logout chrome) --------
  app.registerHandlerViaRegex(
      "^/d/(.*)$",
      [&vault, basePath](const HttpRequestPtr& req,
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
        const bool authenticated = isAuthenticated(req);
        if (parsed.frontMatter.visibility != "public" && !authenticated) {
          // Fail-safe-private: malformed/missing visibility already
          // defaults to "private" inside parseFrontMatter.
          callback(notFound());
          return;
        }

        const std::string csrfToken =
            req->attributes()->get<std::string>(kAttrCsrfToken);
        callback(renderDocument(basePath, docPath, parsed.frontMatter, parsed.body,
                                 authenticated, csrfToken));
      },
      {Get, "wikicore::auth::AuthFilter"});

  // --- GET /edit/{path...} — WYSIWYG edit page, admin-only ---------------
  app.registerHandlerViaRegex(
      "^/edit/(.*)$",
      [&vault, basePath](const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)>&& callback,
               const std::string& docPath) {
        if (!isAuthenticated(req)) {
          callback(HttpResponse::newRedirectionResponse(
              util::withBasePath(basePath, "/login")));
          return;
        }

        const bool isNew = !vault.exists(docPath);
        FrontMatter fm;
        std::string body;
        if (!isNew) {
          try {
            const ParsedDocument parsed = parseFrontMatter(vault.readRaw(docPath));
            fm = parsed.frontMatter;
            body = parsed.body;
          } catch (const std::exception&) {
            callback(notFound());
            return;
          }
        }

        nlohmann::json docData;
        docData["path"] = docPath;
        docData["isNew"] = isNew;
        docData["title"] = fm.title;
        docData["tags"] = fm.tags;
        docData["type"] = fm.type;
        docData["visibility"] = fm.visibility;
        docData["body"] = body;
        // edit.js needs this too — it builds the save-target URL and the
        // post-save redirect itself, client-side, so it needs the same
        // prefix the server-rendered chrome/redirects already carry.
        docData["basePath"] = basePath;
        std::string docDataJson = docData.dump();
        // Escaped so a body containing "</script>" can't break out of the
        // <script type="application/json"> block it's embedded in — valid
        // JSON permits \u-escaping any character, so this never changes
        // what JSON.parse() on the client sees.
        std::string escaped;
        escaped.reserve(docDataJson.size());
        for (char c : docDataJson) {
          if (c == '<') escaped += "\\u003c";
          else escaped += c;
        }

        const std::string pageTitle = isNew ? "New document" : ("Edit \xe2\x80\x94 " + fm.title);

        HttpViewData data;
        data.insert("pageTitle", util::escapeHtml(pageTitle));
        data.insert("docDataJson", escaped);
        // [[key]] interpolation in .csp views does NOT escape (see
        // docs/architecture.md) — basePath comes from config.toml, not a
        // request, but pre-escape anyway for the same reason every other
        // HttpViewData::insert here does: never make an exception "because
        // this one's trusted".
        data.insert("basePath", util::escapeHtml(basePath));
        callback(HttpResponse::newHttpViewResponse("EditPage", data));
      },
      {Get, "wikicore::auth::AuthFilter"});

  // --- POST /api/documents — create --------------------------------------
  app.registerHandler(
      "/api/documents",
      [&documentService](const HttpRequestPtr& req,
                          std::function<void(const HttpResponsePtr&)>&& callback) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        auto json = req->getJsonObject();
        if (!json || !json->isMember("path") || !(*json)["path"].isString() ||
            (*json)["path"].asString().empty()) {
          callback(jsonError(k400BadRequest, "missing or invalid 'path'"));
          return;
        }
        const std::string path = (*json)["path"].asString();
        try {
          const DocumentInput input = parseDocumentInput(*json);
          const DocumentRecord rec = documentService.create(path, input);
          auto resp = jsonOk(rec.path);
          resp->setStatusCode(k201Created);
          callback(resp);
        } catch (const PathTraversalError&) {
          callback(jsonError(k400BadRequest, "invalid path"));
        } catch (const DocumentAlreadyExistsError&) {
          callback(jsonError(k409Conflict, "a document already exists at that path"));
        } catch (const std::exception& e) {
          callback(jsonError(k500InternalServerError, e.what()));
        }
      },
      {Post, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});

  // --- PUT /api/documents/{path...} — update -----------------------------
  app.registerHandlerViaRegex(
      "^/api/documents/(.*)$",
      [&documentService](const HttpRequestPtr& req,
                          std::function<void(const HttpResponsePtr&)>&& callback,
                          const std::string& docPath) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        auto json = req->getJsonObject();
        if (!json) {
          callback(jsonError(k400BadRequest, "expected a JSON body"));
          return;
        }
        try {
          const DocumentInput input = parseDocumentInput(*json);
          const DocumentRecord rec = documentService.update(docPath, input);
          callback(jsonOk(rec.path));
        } catch (const PathTraversalError&) {
          callback(jsonError(k400BadRequest, "invalid path"));
        } catch (const DocumentNotFoundError&) {
          callback(jsonError(k404NotFound, "document not found"));
        } catch (const std::exception& e) {
          callback(jsonError(k500InternalServerError, e.what()));
        }
      },
      {Put, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});

  // --- DELETE /api/documents/{path...} — soft-delete ---------------------
  app.registerHandlerViaRegex(
      "^/api/documents/(.*)$",
      [&documentService](const HttpRequestPtr& req,
                          std::function<void(const HttpResponsePtr&)>&& callback,
                          const std::string& docPath) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        try {
          documentService.softDelete(docPath);
          callback(jsonOk(docPath));
        } catch (const PathTraversalError&) {
          callback(jsonError(k400BadRequest, "invalid path"));
        } catch (const DocumentNotFoundError&) {
          callback(jsonError(k404NotFound, "document not found"));
        } catch (const std::exception& e) {
          callback(jsonError(k500InternalServerError, e.what()));
        }
      },
      {Delete, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});

  // --- POST /api/attachments/{path...} — upload --------------------------
  app.registerHandlerViaRegex(
      "^/api/attachments/(.*)$",
      [&vault, &attachmentService](
          const HttpRequestPtr& req,
          std::function<void(const HttpResponsePtr&)>&& callback,
          const std::string& docPath) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        if (!vault.exists(docPath)) {
          callback(jsonError(k404NotFound, "owning document not found"));
          return;
        }

        MultiPartParser parser;
        if (parser.parse(req) != 0 || parser.getFiles().size() != 1) {
          callback(jsonError(k400BadRequest, "expected exactly one uploaded file"));
          return;
        }
        const auto& file = parser.getFiles()[0];

        try {
          const AttachmentInfo info = attachmentService.store(
              docPath, file.getFileName(), std::string(file.fileContent()));
          Json::Value body;
          body["path"] = info.relativePath;
          body["mimeType"] = info.mimeType;
          body["size"] = static_cast<Json::UInt64>(info.size);
          auto resp = HttpResponse::newHttpJsonResponse(body);
          resp->setStatusCode(k201Created);
          callback(resp);
        } catch (const AttachmentRejectedError& e) {
          callback(jsonError(k400BadRequest, e.what()));
        } catch (const PathTraversalError&) {
          callback(jsonError(k400BadRequest, "invalid path"));
        } catch (const std::exception& e) {
          callback(jsonError(k500InternalServerError, e.what()));
        }
      },
      {Post, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});

  // --- GET /assets/{path...} — serve an attachment, gated through its ---
  // --- owning document's visibility --------------------------------------
  app.registerHandlerViaRegex(
      "^/assets/(.*)$",
      [&vault](const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)>&& callback,
               const std::string& assetPath) {
        const auto ownerPath = owningDocumentFor(assetPath);
        if (!ownerPath) {
          callback(notFound());
          return;
        }

        std::string ownerRaw;
        try {
          ownerRaw = vault.readRaw(*ownerPath);
        } catch (const PathTraversalError&) {
          callback(notFound());
          return;
        } catch (const std::filesystem::filesystem_error&) {
          callback(notFound());
          return;
        }
        const ParsedDocument ownerParsed = parseFrontMatter(ownerRaw);
        if (ownerParsed.frontMatter.visibility != "public" && !isAuthenticated(req)) {
          callback(notFound());
          return;
        }

        std::filesystem::path fullPath;
        try {
          fullPath = vault.pathGuard().resolve(assetPath);
        } catch (const PathTraversalError&) {
          callback(notFound());
          return;
        }
        if (!std::filesystem::exists(fullPath)) {
          callback(notFound());
          return;
        }
        callback(HttpResponse::newFileResponse(fullPath.string()));
      },
      {Get, "wikicore::auth::AuthFilter"});
}

}  // namespace wikicore::controllers
