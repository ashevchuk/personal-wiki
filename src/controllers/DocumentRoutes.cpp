#include "controllers/DocumentRoutes.h"

#include "auth/RequireAdmin.h"
#include "util/MarkdownRenderer.h"
#include "vault/FrontMatter.h"
#include "vault/PathGuard.h"

#include <drogon/HttpResponse.h>
#include <drogon/MultiPart.h>

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

Json::Value tagsToJson(const std::vector<std::string>& tags) {
  Json::Value arr(Json::arrayValue);
  for (const auto& t : tags) arr.append(t);
  return arr;
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
                             AttachmentService& attachmentService) {
  // --- GET /api/documents/{path...} — read --------------------------------
  // Backs BOTH the document view page and the edit page (client-side —
  // see static/js/pages/view.js and edit.js): the edit page treats a 404
  // here as "this is a new, not-yet-saved document" rather than an error.
  // Same fail-safe-private visibility gating /d/{path...} always had:
  // 404, not 403, for a private document to an anonymous caller.
  app.registerHandlerViaRegex(
      "^/api/documents/(.*)$",
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
        const bool authenticated = isAuthenticated(req);
        if (parsed.frontMatter.visibility != "public" && !authenticated) {
          callback(notFound());
          return;
        }

        const FrontMatter& fm = parsed.frontMatter;
        Json::Value body;
        body["path"] = docPath;
        body["title"] = fm.title;
        body["tags"] = tagsToJson(fm.tags);
        body["type"] = fm.type;
        body["visibility"] = fm.visibility;
        body["body"] = parsed.body;
        body["renderedHtml"] = util::renderMarkdownToHtml(parsed.body);
        body["created"] = fm.created;
        body["updated"] = fm.updated;
        callback(HttpResponse::newHttpJsonResponse(body));
      },
      {Get, "wikicore::auth::AuthFilter"});

  // --- GET /api/documents/{path...}/raw — literal file bytes -------------
  // Same read/visibility-gating as above, minus the JSON/HTML render —
  // the exact front-matter + body as stored on disk, for anything that
  // wants the source (was in the original plan's route sketch as
  // GET /api/documents/{id}/raw; built here keyed by path like every
  // other /api/documents/* route, not a separate id-based lookup).
  app.registerHandlerViaRegex(
      "^/api/documents/(.*)/raw$",
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
        if (parsed.frontMatter.visibility != "public" && !isAuthenticated(req)) {
          callback(notFound());
          return;
        }

        auto resp = HttpResponse::newHttpResponse();
        resp->setContentTypeCode(CT_TEXT_PLAIN);
        resp->setBody(raw);
        callback(resp);
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
      [&vault, &attachmentService](const HttpRequestPtr& req,
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

        // Any file type can be uploaded (see AttachmentService — no
        // extension allowlist/blocklist there anymore); this is the
        // actual safety boundary instead: force a download prompt
        // (Content-Disposition: attachment) for anything NOT on a small
        // curated "safe to render inline" list, so a browser navigating
        // straight to this URL can't execute an uploaded .html/.svg/etc
        // as same-origin content. Doesn't restrict what can be
        // uploaded OR downloaded — only whether it's allowed to render
        // inline vs. save-as.
        std::string ext = fullPath.extension().string();
        if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
        const std::string mimeType = attachmentService.mimeTypeForExtension(ext);
        const std::string attachmentName =
            attachmentService.isSafeToRenderInline(ext) ? "" : fullPath.filename().string();
        callback(HttpResponse::newFileResponse(fullPath.string(), attachmentName, CT_CUSTOM,
                                                mimeType));
      },
      {Get, "wikicore::auth::AuthFilter"});
}

}  // namespace wikicore::controllers
