#include "controllers/AgentRoutes.h"

#include "auth/RequireAdmin.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

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

AgentUiContext uiContextFromJson(const Json::Value& json) {
  AgentUiContext ctx;
  const Json::Value* view = &json;
  if (json.isMember("view") && json["view"].isObject()) {
    view = &json["view"];
  }
  if (view->isMember("page") && (*view)["page"].isString()) {
    ctx.page = (*view)["page"].asString();
  }
  if (view->isMember("path") && (*view)["path"].isString()) {
    ctx.path = (*view)["path"].asString();
  }
  if (view->isMember("title") && (*view)["title"].isString()) {
    ctx.title = (*view)["title"].asString();
  }
  return ctx;
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
  if (json.isMember("caretBefore") && json["caretBefore"].isString()) {
    snap.caretBefore = json["caretBefore"].asString();
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
  body["kind"] = view.kind.empty() ? "draft" : view.kind;
  body["title"] = view.title;
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

std::string formatSse(const AgentEvent& ev, std::size_t index) {
  nlohmann::json data = ev.data;
  std::ostringstream os;
  os << "id: " << index << "\n";
  os << "event: " << ev.type << "\n";
  os << "data: " << data.dump() << "\n\n";
  return os.str();
}

std::string formatSse(const AgentSessionView& view, std::size_t index) {
  AgentEvent ev = view.events[index];
  if (ev.type == "draft" && view.draft) {
    ev.data["path"] = view.draft->path;
    ev.data["title"] = view.draft->title;
    ev.data["type"] = view.draft->type;
    ev.data["body"] = view.draft->body;
    ev.data["tags"] = view.draft->tags;
  }
  return formatSse(ev, index);
}

bool sendOnLoop(const std::shared_ptr<ResponseStream>& stream, const std::string& payload) {
  auto* loop = drogon::app().getLoop();
  if (!loop) return stream->send(payload);
  auto done = std::make_shared<std::promise<bool>>();
  auto future = done->get_future();
  loop->queueInLoop([stream, payload, done]() {
    try {
      done->set_value(stream->send(payload));
    } catch (...) {
      done->set_value(false);
    }
  });
  return future.get();
}

void closeOnLoop(const std::shared_ptr<ResponseStream>& stream) {
  auto* loop = drogon::app().getLoop();
  if (!loop) {
    stream->close();
    return;
  }
  loop->queueInLoop([stream]() { stream->close(); });
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
        std::string kind = "draft";
        if (json->isMember("kind") && (*json)["kind"].isString()) {
          kind = (*json)["kind"].asString();
        }
        try {
          std::string id;
          if (kind == "chat") {
            id = agent.startChat(instruction, uiContextFromJson(*json));
          } else if (kind == "draft") {
            id = agent.start(instruction, snapshotFromJson(*json));
          } else {
            callback(jsonError(k400BadRequest, "kind must be draft or chat"));
            return;
          }
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
        Json::Value body;
        Json::Value sessions(Json::arrayValue);
        for (const auto& row : agent.listChats()) {
          Json::Value item;
          item["id"] = row.id;
          item["title"] = row.title;
          item["createdAt"] = row.createdAt;
          item["updatedAt"] = row.updatedAt;
          sessions.append(item);
        }
        body["sessions"] = sessions;
        callback(HttpResponse::newHttpJsonResponse(body));
      },
      {Get, "wikicore::auth::AuthFilter"});

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
          agent.loadChat(sessionId);
          const auto existing = agent.view(sessionId);
          if (!existing) {
            callback(jsonError(k404NotFound, "session not found"));
            return;
          }
          if (existing->kind == "chat") {
            agent.sendChat(sessionId, (*json)["instruction"].asString(),
                           uiContextFromJson(*json));
          } else {
            agent.send(sessionId, (*json)["instruction"].asString(),
                       snapshotFromJson(*json));
          }
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
      "^/api/agent/sessions/([^/]+)/stream$",
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
        if (!agent.loadChat(sessionId) && !agent.view(sessionId)) {
          callback(jsonError(k404NotFound, "session not found"));
          return;
        }
        std::size_t after = 0;
        const std::string afterStr = req->getParameter("after");
        if (!afterStr.empty()) {
          try {
            after = static_cast<std::size_t>(std::stoull(afterStr));
          } catch (const std::exception&) {
            after = 0;
          }
        }
        auto resp = HttpResponse::newAsyncStreamResponse(
            [&agent, sessionId, after](ResponseStreamPtr stream) {
              auto shared = std::shared_ptr<ResponseStream>(std::move(stream));
              std::thread([&agent, sessionId, after, shared]() {
                std::size_t sent = after;
                std::string lastAssistantKey;
                auto assistantKey = [](const AgentEvent& ev) {
                  return ev.data.value("text", std::string()) +
                         (ev.data.value("streaming", false) ? "|s" : "");
                };
                try {
                  while (true) {
                    const auto view = agent.view(sessionId);
                    if (!view) break;
                    const auto& events = view->events;
                    for (std::size_t i = sent; i < events.size(); ++i) {
                      if (!sendOnLoop(shared, formatSse(*view, i))) goto closed;
                      if (events[i].type == "assistant") {
                        lastAssistantKey = assistantKey(events[i]);
                      }
                    }
                    sent = events.size();
                    if (sent > 0 && events[sent - 1].type == "assistant") {
                      const auto key = assistantKey(events[sent - 1]);
                      if (key != lastAssistantKey) {
                        if (!sendOnLoop(shared, formatSse(*view, sent - 1))) {
                          goto closed;
                        }
                        lastAssistantKey = key;
                      }
                    }
                    if (view->status != "running") break;
                    const auto gen = agent.generation(sessionId);
                    if (!agent.waitGeneration(sessionId, gen, std::chrono::seconds(15))) {
                      if (!sendOnLoop(shared, ": keepalive\n\n")) break;
                    }
                  }
                } catch (...) {
                }
              closed:
                closeOnLoop(shared);
              }).detach();
            },
            true);
        resp->setContentTypeCodeAndCustomString(
            CT_CUSTOM, "text/event-stream; charset=utf-8");
        resp->addHeader("Cache-Control", "no-cache");
        resp->addHeader("Connection", "keep-alive");
        resp->addHeader("X-Accel-Buffering", "no");
        resp->setAllowCompression(false);
        callback(resp);
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
        agent.loadChat(sessionId);
        const auto view = agent.view(sessionId);
        if (!view) {
          callback(jsonError(k404NotFound, "session not found"));
          return;
        }
        callback(HttpResponse::newHttpJsonResponse(sessionToJson(*view)));
      },
      {Get, "wikicore::auth::AuthFilter"});

  app.registerHandlerViaRegex(
      "^/api/agent/sessions/([^/]+)/title$",
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
        if (!json || !json->isMember("title") || !(*json)["title"].isString()) {
          callback(jsonError(k400BadRequest, "expected {title: string}"));
          return;
        }
        try {
          agent.renameChat(sessionId, (*json)["title"].asString());
          Json::Value body;
          body["ok"] = true;
          body["title"] = (*json)["title"].asString();
          callback(HttpResponse::newHttpJsonResponse(body));
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
        agent.drop(sessionId);
        Json::Value body;
        body["ok"] = true;
        callback(HttpResponse::newHttpJsonResponse(body));
      },
      {Delete, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});
}

}  // namespace wikicore::controllers
