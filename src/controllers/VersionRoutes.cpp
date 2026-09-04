#include "controllers/VersionRoutes.h"

#include "auth/RequireAdmin.h"
#include "vault/FrontMatter.h"

#include <drogon/HttpResponse.h>

#include <optional>

using namespace drogon;
using namespace wikicore::auth;
using namespace wikicore::index;
using namespace wikicore::vault;

namespace wikicore::controllers {

namespace {

HttpResponsePtr jsonError(HttpStatusCode status, const std::string& message) {
  Json::Value body;
  body["error"] = message;
  auto resp = HttpResponse::newHttpJsonResponse(body);
  resp->setStatusCode(status);
  return resp;
}

Json::Value tagsToJson(const std::vector<std::string>& tags) {
  Json::Value arr(Json::arrayValue);
  for (const auto& t : tags) arr.append(t);
  return arr;
}

// A snapshot id that isn't a positive integer at all (missing, empty,
// garbage) is treated the same as "no id given" by the history route —
// distinguishing "malformed" from "absent" would only matter if this
// route did anything different for the two, which it doesn't. The
// restore route DOES need to tell the two apart (a malformed id there
// is a real 400, not "list mode" — there's no list mode to fall back
// to), so it checks parsed.has_value() itself instead of reusing this
// as a bool.
std::optional<int64_t> parseIdParam(const HttpRequestPtr& req) {
  const std::string raw = req->getParameter("id");
  if (raw.empty()) return std::nullopt;
  try {
    size_t consumed = 0;
    const long long value = std::stoll(raw, &consumed);
    if (consumed != raw.size() || value <= 0) return std::nullopt;
    return static_cast<int64_t>(value);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace

void registerVersionRoutes(HttpAppFramework& app, IndexUpdater& indexUpdater,
                            SnapshotStore& snapshots, DocumentService& documentService) {
  // --- GET /api/document-history/{path...}[?id=N] -------------------------
  app.registerHandlerViaRegex(
      "^/api/document-history/(.*)$",
      [&indexUpdater, &snapshots](const HttpRequestPtr& req,
                                   std::function<void(const HttpResponsePtr&)>&& callback,
                                   const std::string& docPath) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        const auto rowId = indexUpdater.rowIdForPath(docPath);
        if (!rowId) {
          callback(jsonError(k404NotFound, "document not indexed"));
          return;
        }

        const auto snapshotId = parseIdParam(req);
        if (!snapshotId) {
          // List mode — every recorded past state, newest first.
          Json::Value arr(Json::arrayValue);
          for (const auto& s : snapshots.list(*rowId)) {
            Json::Value item;
            item["id"] = static_cast<Json::Int64>(s.id);
            item["snapshotAt"] = s.snapshotAt;
            arr.append(item);
          }
          Json::Value body;
          body["snapshots"] = arr;
          callback(HttpResponse::newHttpJsonResponse(body));
          return;
        }

        // Single-snapshot mode — parsed the same shape as GET
        // /api/documents/{path}'s own response, so the frontend's diff
        // view can treat "the live document" and "a past snapshot" as
        // interchangeable inputs to the same diff function.
        const auto content = snapshots.getContent(*rowId, *snapshotId);
        if (!content) {
          callback(jsonError(k404NotFound, "no such snapshot for this document"));
          return;
        }
        const ParsedDocument parsed = parseFrontMatter(*content);
        Json::Value body;
        body["id"] = static_cast<Json::Int64>(*snapshotId);
        body["title"] = parsed.frontMatter.title;
        body["tags"] = tagsToJson(parsed.frontMatter.tags);
        body["type"] = parsed.frontMatter.type;
        body["visibility"] = parsed.frontMatter.visibility;
        body["body"] = parsed.body;
        callback(HttpResponse::newHttpJsonResponse(body));
      },
      {Get, "wikicore::auth::AuthFilter"});

  // --- POST /api/document-restore/{path...}?id=N --------------------------
  app.registerHandlerViaRegex(
      "^/api/document-restore/(.*)$",
      [&indexUpdater, &snapshots, &documentService](
          const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback,
          const std::string& docPath) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        const auto snapshotId = parseIdParam(req);
        if (!snapshotId) {
          callback(jsonError(k400BadRequest, "missing or invalid 'id' query parameter"));
          return;
        }
        const auto rowId = indexUpdater.rowIdForPath(docPath);
        if (!rowId) {
          callback(jsonError(k404NotFound, "document not indexed"));
          return;
        }
        const auto content = snapshots.getContent(*rowId, *snapshotId);
        if (!content) {
          callback(jsonError(k404NotFound, "no such snapshot for this document"));
          return;
        }

        const ParsedDocument parsed = parseFrontMatter(*content);
        DocumentInput input;
        input.title = parsed.frontMatter.title;
        input.tags = parsed.frontMatter.tags;
        input.visibility = parsed.frontMatter.visibility;
        input.type = parsed.frontMatter.type;
        input.body = parsed.body;

        try {
          // update(), not create() — restoring only ever makes sense
          // against a document that still exists (its row is how
          // `rowId`/`content` were found in the first place). This also
          // means the restore itself gets snapshotted as "the state
          // right before the restore" — undoing a restore is just
          // restoring THAT snapshot.
          const DocumentRecord rec = documentService.update(docPath, input);
          Json::Value body;
          body["path"] = rec.path;
          callback(HttpResponse::newHttpJsonResponse(body));
        } catch (const DocumentNotFoundError&) {
          callback(jsonError(k404NotFound, "document not found"));
        } catch (const std::exception& e) {
          callback(jsonError(k500InternalServerError, e.what()));
        }
      },
      {Post, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});
}

}  // namespace wikicore::controllers
