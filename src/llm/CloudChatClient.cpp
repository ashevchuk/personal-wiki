#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include "llm/CloudChatClient.h"
#include "llm/ChatStreamParser.h"

#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

namespace wikicore::llm {

namespace {

constexpr const char* kDefaultApiBase = "https://api.openai.com/v1";
constexpr const char* kDefaultModel = "gpt-4.1-mini";

struct ParsedBase {
  std::string origin;
  std::string chatPath;
};

ParsedBase parseApiBase(std::string base) {
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  if (base.empty()) {
    base = kDefaultApiBase;
  }

  const auto schemeEnd = base.find("://");
  if (schemeEnd == std::string::npos) {
    throw std::runtime_error(
        "CloudChatClient: llm.api_base must be an absolute http(s) URL (got \"" +
        base + "\")");
  }
  const std::string scheme = base.substr(0, schemeEnd);
  if (scheme != "http" && scheme != "https") {
    throw std::runtime_error(
        "CloudChatClient: llm.api_base must use http or https (got scheme \"" +
        scheme + "\")");
  }
  const std::string rest = base.substr(schemeEnd + 3);
  if (rest.empty()) {
    throw std::runtime_error("CloudChatClient: llm.api_base has no host (got \"" +
                              base + "\")");
  }
  const auto slash = rest.find('/');
  const std::string hostport =
      slash == std::string::npos ? rest : rest.substr(0, slash);
  const std::string pathPrefix =
      slash == std::string::npos ? std::string() : rest.substr(slash);
  if (hostport.empty()) {
    throw std::runtime_error("CloudChatClient: llm.api_base has no host (got \"" +
                              base + "\")");
  }
  ParsedBase out;
  out.origin = scheme + "://" + hostport;
  out.chatPath = pathPrefix + "/chat/completions";
  return out;
}

nlohmann::json messageToJson(const ChatMessage& msg) {
  nlohmann::json j;
  j["role"] = msg.role;
  if (msg.role == "tool") {
    j["tool_call_id"] = msg.toolCallId;
    j["content"] = msg.content;
    return j;
  }
  if (!msg.toolCalls.empty()) {
    nlohmann::json calls = nlohmann::json::array();
    for (const auto& call : msg.toolCalls) {
      calls.push_back({{"id", call.id},
                       {"type", "function"},
                       {"function", {{"name", call.name}, {"arguments", call.arguments}}}});
    }
    j["tool_calls"] = calls;
    if (!msg.content.empty()) j["content"] = msg.content;
    else j["content"] = nullptr;
    return j;
  }
  j["content"] = msg.content;
  return j;
}

}  // namespace

CloudChatClient::CloudChatClient(const std::string& apiKeyEnvVar, std::string apiBase,
                                 std::string model) {
  if (!apiKeyEnvVar.empty()) {
    const char* key = std::getenv(apiKeyEnvVar.c_str());
    if (key == nullptr || key[0] == '\0') {
      throw std::runtime_error(
          "CloudChatClient: environment variable '" + apiKeyEnvVar +
          "' (named by llm.api_key_env) is not set");
    }
    apiKey_ = key;
  }

  const auto parsed = parseApiBase(std::move(apiBase));
  origin_ = parsed.origin;
  chatPath_ = parsed.chatPath;
  model_ = model.empty() ? kDefaultModel : std::move(model);
}

void CloudChatClient::cancel() {
  stopRequested_.store(true);
  std::lock_guard<std::mutex> lock(requestMu_);
  if (auto* client = static_cast<httplib::Client*>(activeClient_)) {
    client->stop();
  }
}

ChatCompletion CloudChatClient::complete(const std::vector<ChatMessage>& messages,
                                         const nlohmann::json& tools,
                                         const ChatDeltaFn& onDelta) {
  nlohmann::json msgs = nlohmann::json::array();
  for (const auto& msg : messages) msgs.push_back(messageToJson(msg));

  auto run = [&](bool stream) -> ChatCompletion {
    stopRequested_.store(false);
    httplib::Client client(origin_);
    if (!apiKey_.empty()) {
      client.set_bearer_token_auth(apiKey_);
    }
    client.set_connection_timeout(15);
    client.set_read_timeout(stream ? 120 : 90);

    nlohmann::json body;
    body["model"] = model_;
    body["max_tokens"] = 8192;
    body["temperature"] = 0.3;
    body["messages"] = msgs;
    if (!tools.is_null() && !tools.empty()) body["tools"] = tools;
    if (stream) body["stream"] = true;

    ChatStreamParser parser;
    std::string errorBody;
    int status = 0;
    std::exception_ptr eptr;

    httplib::Request req;
    req.method = "POST";
    req.path = chatPath_;
    req.body = body.dump();
    req.set_header("Content-Type", "application/json");
    if (stream) req.set_header("Accept", "text/event-stream");
    req.response_handler = [&](const httplib::Response& res) {
      status = res.status;
      return true;
    };
    req.content_receiver = [&](const char* data, size_t len, uint64_t, uint64_t) {
      if (stopRequested_.load()) return false;
      if (status != 0 && status != 200) {
        errorBody.append(data, len);
        return true;
      }
      try {
        parser.feed(std::string_view(data, len), stream ? onDelta : ChatDeltaFn{});
      } catch (...) {
        eptr = std::current_exception();
        return false;
      }
      return true;
    };

    {
      std::lock_guard<std::mutex> lock(requestMu_);
      if (stopRequested_.load()) {
        throw std::runtime_error("cancelled");
      }
      activeClient_ = &client;
    }
    const auto res = client.send(req);
    {
      std::lock_guard<std::mutex> lock(requestMu_);
      activeClient_ = nullptr;
    }
    if (eptr) std::rethrow_exception(eptr);
    if (stopRequested_.load() || res.error() == httplib::Error::Canceled) {
      throw std::runtime_error("cancelled");
    }
    if (res.error() != httplib::Error::Success) {
      throw std::runtime_error("CloudChatClient: HTTP request failed (" +
                                httplib::to_string(res.error()) + ")");
    }
    const int httpStatus = status != 0 ? status : (res ? res->status : 0);
    if (httpStatus != 200) {
      if (errorBody.empty() && res) errorBody = res->body;
      throw std::runtime_error("CloudChatClient: chat API returned HTTP " +
                                std::to_string(httpStatus) + ": " + errorBody);
    }
    ChatCompletion out;
    try {
      out = parser.finish(stream ? onDelta : ChatDeltaFn{});
    } catch (const nlohmann::json::parse_error& e) {
      throw std::runtime_error(std::string("CloudChatClient: failed to parse response: ") +
                                e.what());
    }
    if (!stream && onDelta && !out.content.empty() && out.toolCalls.empty()) {
      onDelta(out.content);
    }
    return out;
  };

  try {
    return run(true);
  } catch (const std::runtime_error& e) {
    const std::string msg = e.what();
    const bool http4xx = msg.find("chat API returned HTTP 4") != std::string::npos;
    if (!http4xx) throw;
    return run(false);
  }
}

}  // namespace wikicore::llm
