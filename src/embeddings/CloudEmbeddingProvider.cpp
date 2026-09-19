// CPPHTTPLIB_OPENSSL_SUPPORT is defined here, in this ONE translation
// unit, before including the vendored header — see this project's root
// CMakeLists.txt "Cloud embedding provider" comment for why: cpp-mcp
// itself builds httplib.h without SSL support (it doesn't need outbound
// HTTPS), but the header is header-only, so this file can opt itself in
// without touching cpp-mcp's own build.
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include "embeddings/CloudEmbeddingProvider.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <stdexcept>

namespace wikicore::embeddings {

namespace {
constexpr const char* kApiHost = "https://api.openai.com";
constexpr const char* kEmbeddingsPath = "/v1/embeddings";
// text-embedding-3-small: OpenAI's cheapest current embeddings model,
// 1536-dim by default. Not user-configurable yet — see docs/embeddings.md's
// phased rollout; a fixed model keeps this first cloud pass simple, same
// spirit as LocalEmbeddingProvider not yet supporting per-call options.
constexpr const char* kModel = "text-embedding-3-small";
constexpr std::size_t kDimensions = 1536;
}  // namespace

CloudEmbeddingProvider::CloudEmbeddingProvider(const std::string& apiKeyEnvVar) {
  const char* key = std::getenv(apiKeyEnvVar.c_str());
  if (key == nullptr || key[0] == '\0') {
    throw std::runtime_error(
        "CloudEmbeddingProvider: environment variable '" + apiKeyEnvVar +
        "' (named by embeddings.api_key_env) is not set");
  }
  apiKey_ = key;
}

std::vector<float> CloudEmbeddingProvider::embed(const std::string& text) {
  httplib::Client client(kApiHost);
  client.set_bearer_token_auth(apiKey_);
  client.set_connection_timeout(10);
  client.set_read_timeout(30);

  nlohmann::json requestBody;
  requestBody["model"] = kModel;
  requestBody["input"] = text;

  const auto res = client.Post(kEmbeddingsPath, requestBody.dump(), "application/json");
  if (!res) {
    throw std::runtime_error("CloudEmbeddingProvider::embed: HTTP request failed (" +
                              httplib::to_string(res.error()) + ")");
  }
  if (res->status != 200) {
    throw std::runtime_error("CloudEmbeddingProvider::embed: OpenAI API returned HTTP " +
                              std::to_string(res->status) + ": " + res->body);
  }

  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(res->body);
  } catch (const nlohmann::json::parse_error& e) {
    throw std::runtime_error(
        std::string("CloudEmbeddingProvider::embed: failed to parse response JSON: ") +
        e.what());
  }

  if (!parsed.contains("data") || !parsed["data"].is_array() || parsed["data"].empty()) {
    throw std::runtime_error(
        "CloudEmbeddingProvider::embed: response has no usable 'data' array");
  }
  const auto& first = parsed["data"][0];
  if (!first.contains("embedding") || !first["embedding"].is_array()) {
    throw std::runtime_error(
        "CloudEmbeddingProvider::embed: response 'data[0].embedding' is missing or not "
        "an array");
  }

  std::vector<float> result;
  result.reserve(first["embedding"].size());
  for (const auto& value : first["embedding"]) {
    result.push_back(value.get<float>());
  }

  if (result.size() != kDimensions) {
    throw std::runtime_error("CloudEmbeddingProvider::embed: expected " +
                              std::to_string(kDimensions) + " dimensions, got " +
                              std::to_string(result.size()));
  }

  return result;
}

std::size_t CloudEmbeddingProvider::dimensions() const { return kDimensions; }

std::string CloudEmbeddingProvider::modelIdentifier() const {
  return std::string("cloud:") + kModel;
}

}  // namespace wikicore::embeddings
