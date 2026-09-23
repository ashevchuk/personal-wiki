#include "controllers/AgentRoutes.h"

#include "auth/RequireAdmin.h"

#include <drogon/HttpResponse.h>

#include <memory>

using namespace drogon;
using namespace wikicore::auth;
using namespace wikicore::llm;

namespace wikicore::controllers {

namespace {

HttpResponsePtr jsonError(HttpStatusCode status, const std::string& message) {
  Json::Value body;
  body["error"] = message;
  auto resp = HttpResponse::newHttpJsonResponse(body);
  resp->setStatusCode(status);
  return resp;
}

Json::Value nlohmannToJsonCpp(const nlohmann::json& j) {
  Json::Value v;
  Json::CharReaderBuilder builder;
  std::string errs;
  const std::string dumped = j.dump();
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (!reader->parse(dumped.data(), dumped.data() + dumped.size(), &v, &errs)) {
    v = Json::Value(dumped);
  }
  return v;
}

AgentDocumentSnapshot snapshotFromJson(const Json::Value& json) {
  AgentDocumentSnapshot snap;
  if (json.isMember("path") && json["path"].isString()) snap.path = json["path"].asString();
  if (json.isMember("title") && json["title"].isString()) snap.title = json["title"].asString();
  if (json.isMember("type") && json["type"].isString()) snap.type = json["type"].asString();
  if (json.isMember("body") && json["body"].isString()) snap.body = json["body"].asString();
  if (json.isMember("selection") && json["selection"].isString()) {
    snap.selection = json["selection"].asString();
  }
  if (json.isMember("isNew") && json["isNew"].isBool()) snap.isNew = json["isNew"].asBool();
  if (json.isMember("tags") && json["tags"].isArray()) {
    for (const auto& t : json["tags"]) {
      if (t.isString()) snap.tags.push_back(t.asString());
    }
  }
  return snap;
}

Json::Value sessionToJson(const AgentSessionView& view) {
  Json::Value body;
  body["id"] = view.id;
  body["status"] = view.status;
  Json::Value events(Json::arrayValue);
  for (const auto& ev : view.events) {
    Json::Value row;
    row["type"] = ev.type;
    row["data"] = nlohmannToJsonCpp(ev.data);
    events.append(row);
  }
  body["events"] = events;
  if (view.draft) {
    Json::Value draft;
    draft["path"] = view.draft->path;
    draft["title"] = view.draft->title;
    draft["type"] = view.draft->type;
    draft["body"] = view.draft->body;
    Json::Value tags(Json::arrayValue);
    for (const auto& t : view.draft->tags) tags.append(t);
    draft["tags"] = tags;
    body["draft"] = draft;
  }
  return body;
}

}  // namespace

void registerAgentRoutes(HttpAppFramework& app, AgentRuntime& agent) {
  app.registerHandler(
      "/api/agent/sessions",
      [&agent](const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)>&& callback) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        if (!agent.enabled()) {
          callback(jsonError(k404NotFound, "agent not configured"));
          return;
        }
        auto json = req->getJsonObject();
        if (!json || !json->isMember("instruction") || !(*json)["instruction"].isString()) {
          callback(jsonError(k400BadRequest, "expected {instruction: string}"));
          return;
        }
        const std::string instruction = (*json)["instruction"].asString();
        try {
          const std::string id = agent.start(instruction, snapshotFromJson(*json));
          const auto view = agent.view(id);
          callback(HttpResponse::newHttpJsonResponse(sessionToJson(*view)));
        } catch (const std::exception& e) {
          const std::string msg = e.what();
          if (msg == "an agent session is already running") {
            callback(jsonError(k409Conflict, msg));
            return;
          }
          callback(jsonError(k400BadRequest, msg));
        }
      },
      {Post, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});

  app.registerHandlerViaRegex(
      "^/api/agent/sessions/([^/]+)/messages$",
      [&agent](const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)>&& callback,
               const std::string& sessionId) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        if (!agent.enabled()) {
          callback(jsonError(k404NotFound, "agent not configured"));
          return;
        }
        auto json = req->getJsonObject();
        if (!json || !json->isMember("instruction") || !(*json)["instruction"].isString()) {
          callback(jsonError(k400BadRequest, "expected {instruction: string}"));
          return;
        }
        try {
          agent.send(sessionId, (*json)["instruction"].asString(), snapshotFromJson(*json));
          const auto view = agent.view(sessionId);
          if (!view) {
            callback(jsonError(k404NotFound, "session not found"));
            return;
          }
          callback(HttpResponse::newHttpJsonResponse(sessionToJson(*view)));
        } catch (const std::exception& e) {
          const std::string msg = e.what();
          if (msg == "session not found") {
            callback(jsonError(k404NotFound, msg));
            return;
          }
          if (msg == "session is still running" || msg == "an agent session is already running") {
            callback(jsonError(k409Conflict, msg));
            return;
          }
          callback(jsonError(k400BadRequest, msg));
        }
      },
      {Post, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});

  app.registerHandlerViaRegex(
      "^/api/agent/sessions/([^/]+)/cancel$",
      [&agent](const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)>&& callback,
               const std::string& sessionId) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        if (!agent.enabled()) {
          callback(jsonError(k404NotFound, "agent not configured"));
          return;
        }
        try {
          agent.cancel(sessionId);
          const auto view = agent.view(sessionId);
          if (!view) {
            callback(jsonError(k404NotFound, "session not found"));
            return;
          }
          callback(HttpResponse::newHttpJsonResponse(sessionToJson(*view)));
        } catch (const std::exception& e) {
          const std::string msg = e.what();
          if (msg == "session not found") {
            callback(jsonError(k404NotFound, msg));
            return;
          }
          callback(jsonError(k400BadRequest, msg));
        }
      },
      {Post, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});

  app.registerHandlerViaRegex(
      "^/api/agent/sessions/([^/]+)$",
      [&agent](const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)>&& callback,
               const std::string& sessionId) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        if (!agent.enabled()) {
          callback(jsonError(k404NotFound, "agent not configured"));
          return;
        }
        const auto view = agent.view(sessionId);
        if (!view) {
          callback(jsonError(k404NotFound, "session not found"));
          return;
        }
        callback(HttpResponse::newHttpJsonResponse(sessionToJson(*view)));
      },
      {Get, "wikicore::auth::AuthFilter"});

  app.registerHandlerViaRegex(
      "^/api/agent/sessions/([^/]+)$",
      [&agent](const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)>&& callback,
               const std::string& sessionId) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        if (!agent.enabled()) {
          callback(jsonError(k404NotFound, "agent not configured"));
          return;
        }
        agent.drop(sessionId);
        Json::Value body;
        body["ok"] = true;
        callback(HttpResponse::newHttpJsonResponse(body));
      },
      {Delete, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});
}

}  // namespace wikicore::controllers
