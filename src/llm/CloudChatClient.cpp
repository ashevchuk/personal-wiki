#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include "llm/CloudChatClient.h"

#include <cstdlib>
#include <stdexcept>
#include <string>

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
                                         const nlohmann::json& tools) {
  stopRequested_.store(false);
  httplib::Client client(origin_);
  if (!apiKey_.empty()) {
    client.set_bearer_token_auth(apiKey_);
  }
  client.set_connection_timeout(15);
  client.set_read_timeout(90);

  nlohmann::json body;
  body["model"] = model_;
  body["max_tokens"] = 8192;
  body["temperature"] = 0.3;
  nlohmann::json msgs = nlohmann::json::array();
  for (const auto& msg : messages) msgs.push_back(messageToJson(msg));
  body["messages"] = msgs;
  if (!tools.is_null() && !tools.empty()) body["tools"] = tools;

  {
    std::lock_guard<std::mutex> lock(requestMu_);
    if (stopRequested_.load()) {
      throw std::runtime_error("cancelled");
    }
    activeClient_ = &client;
  }
  const auto res = client.Post(chatPath_, body.dump(), "application/json");
  {
    std::lock_guard<std::mutex> lock(requestMu_);
    activeClient_ = nullptr;
  }
  if (stopRequested_.load() || (!res && res.error() == httplib::Error::Canceled)) {
    throw std::runtime_error("cancelled");
  }
  if (!res) {
    throw std::runtime_error("CloudChatClient: HTTP request failed (" +
                              httplib::to_string(res.error()) + ")");
  }
  if (res->status != 200) {
    throw std::runtime_error("CloudChatClient: chat API returned HTTP " +
                              std::to_string(res->status) + ": " + res->body);
  }

  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(res->body);
  } catch (const nlohmann::json::parse_error& e) {
    throw std::runtime_error(std::string("CloudChatClient: failed to parse response: ") +
                              e.what());
  }
  if (!parsed.contains("choices") || !parsed["choices"].is_array() ||
      parsed["choices"].empty()) {
    throw std::runtime_error("CloudChatClient: response has no choices");
  }
  const auto& message = parsed["choices"][0]["message"];
  ChatCompletion out;
  if (message.contains("content") && message["content"].is_string()) {
    out.content = message["content"].get<std::string>();
  }
  if (parsed["choices"][0].contains("finish_reason") &&
      parsed["choices"][0]["finish_reason"].is_string()) {
    out.finishReason = parsed["choices"][0]["finish_reason"].get<std::string>();
  }
  if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
    for (const auto& call : message["tool_calls"]) {
      ToolCall tc;
      tc.id = call.value("id", "");
      if (call.contains("function") && call["function"].is_object()) {
        tc.name = call["function"].value("name", "");
        if (call["function"].contains("arguments")) {
          if (call["function"]["arguments"].is_string()) {
            tc.arguments = call["function"]["arguments"].get<std::string>();
          } else {
            tc.arguments = call["function"]["arguments"].dump();
          }
        }
      }
      out.toolCalls.push_back(std::move(tc));
    }
  }
  return out;
}

}  // namespace wikicore::llm
