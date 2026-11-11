#include "controllers/RemoteAgentRoutes.h"

#include "auth/ClientIp.h"
#include "llm/RemoteAgentRuntime.h"

#include <drogon/HttpResponse.h>

#include <string_view>

using namespace drogon;
using namespace wikicore::auth;
using namespace wikicore::llm;

namespace wikicore::controllers {

namespace {

HttpResponsePtr jsonErrorResponse(HttpStatusCode status, const std::string& message) {
  Json::Value body;
  body["error"] = message;
  auto resp = HttpResponse::newHttpJsonResponse(body);
  resp->setStatusCode(status);
  return resp;
}

}  // namespace

void registerRemoteAgentRoutes(HttpAppFramework& app, McpRemoteConfig& remoteConfig,
                                RateLimiter& rateLimiter, ChatClient* chat,
                                vault::DocumentService& documents, index::FtsSearch& search,
                                index::NavQueries& nav, index::CalendarQueries& calendar,
                                index::IndexUpdater& indexUpdater, index::McpAuditLog& auditLog,
                                std::string systemPrompt, bool trustProxyHeaders) {
  app.registerHandler(
      "/mcp/agent",
      [&remoteConfig, &rateLimiter, chat, &documents, &search, &nav, &calendar, &indexUpdater,
       &auditLog, systemPrompt,
       trustProxyHeaders](const HttpRequestPtr& req,
                           std::function<void(const HttpResponsePtr&)>&& callback) {
        // Same "feature off -> plain 404" discipline as POST /mcp -- no
        // hint to an anonymous prober that a token would even help.
        const auth::RemoteMcpSettings settings = remoteConfig.get();
        if (!settings.enabled) {
          auto resp = HttpResponse::newHttpResponse();
          resp->setStatusCode(k404NotFound);
          callback(resp);
          return;
        }

        const std::string ip = clientIp(req, trustProxyHeaders);
        if (!rateLimiter.allow(ip)) {
          auto resp = HttpResponse::newHttpResponse();
          resp->setStatusCode(k429TooManyRequests);
          callback(resp);
          return;
        }

        // Before the token, same reasoning as POST /mcp.
        if (!remoteConfig.isIpAllowed(ip)) {
          auto resp = HttpResponse::newHttpResponse();
          resp->setStatusCode(k403Forbidden);
          callback(resp);
          return;
        }

        const std::string authHeader = req->getHeader("Authorization");
        constexpr std::string_view kBearerPrefix = "Bearer ";
        const bool hasBearer = authHeader.size() > kBearerPrefix.size() &&
                                authHeader.compare(0, kBearerPrefix.size(), kBearerPrefix) == 0;
        const std::string token = hasBearer ? authHeader.substr(kBearerPrefix.size()) : "";
        const auto match = hasBearer ? remoteConfig.verifyToken(token)
                                      : McpRemoteConfig::TokenMatch{};
        if (!match.matched) {
          rateLimiter.recordFailure(ip);
          auto resp = HttpResponse::newHttpResponse();
          resp->setStatusCode(k401Unauthorized);
          callback(resp);
          return;
        }
        rateLimiter.recordSuccess(ip);

        // Past this point the caller has already proven a valid
        // credential, so "not available" gets a real status/message
        // rather than the anonymous-prober-facing 404 above. Permission
        // (403) is checked BEFORE server configuration state (503) --
        // a token without agentAccess must never learn whether an LLM
        // happens to be configured; that's not this token's business
        // either way.
        if (!settings.writeEnabled) {
          callback(jsonErrorResponse(k403Forbidden,
                                      "write access is disabled for remote MCP"));
          return;
        }
        if (!match.agentAccess) {
          callback(jsonErrorResponse(k403Forbidden, "agent access is disabled for this token"));
          return;
        }
        if (chat == nullptr) {
          callback(jsonErrorResponse(k503ServiceUnavailable,
                                      "no LLM is configured on this server ([llm].provider)"));
          return;
        }

        auto json = req->getJsonObject();
        if (!json || !json->isMember("messages") || !(*json)["messages"].isArray() ||
            (*json)["messages"].empty()) {
          callback(jsonErrorResponse(k400BadRequest, "expected {\"messages\": [...]}"));
          return;
        }

        std::vector<ChatMessage> history;
        for (const auto& m : (*json)["messages"]) {
          if (!m.isMember("role") || !m.isMember("content") || !m["role"].isString() ||
              !m["content"].isString()) {
            continue;
          }
          const std::string role = m["role"].asString();
          // Only plain conversational turns from the client -- "system"
          // and "tool" are this server's own internal bookkeeping for one
          // call, never something a caller gets to inject.
          if (role != "user" && role != "assistant") continue;
          history.push_back(ChatMessage{role, m["content"].asString(), "", {}});
        }
        if (history.empty()) {
          callback(jsonErrorResponse(
              k400BadRequest, "messages must contain at least one user/assistant turn"));
          return;
        }

        RemoteAgentRuntime runtime(*chat, documents, search, nav, calendar, indexUpdater,
                                    auditLog, systemPrompt);
        RemoteAgentTurn turn;
        try {
          turn = runtime.run(history, match.label);
        } catch (const std::exception&) {
          // Full detail already went to mcp_audit_log inside run() --
          // the LLM provider's own raw error body stays there, not on
          // this response, same discipline as filesystem_error never
          // reaching a remote caller elsewhere in this app.
          callback(jsonErrorResponse(k502BadGateway, "agent request failed"));
          return;
        }

        Json::Value body;
        body["reply"] = turn.reply;
        Json::Value created(Json::arrayValue);
        for (const auto& p : turn.documentsCreated) created.append(p);
        body["documentsCreated"] = created;
        callback(HttpResponse::newHttpJsonResponse(body));
      },
      {Post});
}

}  // namespace wikicore::controllers
