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
constexpr const char* kDefaultApiBase = "https://api.openai.com/v1";
constexpr const char* kDefaultModel = "text-embedding-3-small";
constexpr std::size_t kDefaultDimensions = 1536;

struct ParsedBase {
  std::string origin;
  std::string embeddingsPath;
  std::string normalized;
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
        "CloudEmbeddingProvider: embeddings.api_base must be an absolute "
        "http(s) URL (got \"" +
        base + "\")");
  }
  const std::string scheme = base.substr(0, schemeEnd);
  if (scheme != "http" && scheme != "https") {
    throw std::runtime_error(
        "CloudEmbeddingProvider: embeddings.api_base must use http or https "
        "(got scheme \"" +
        scheme + "\")");
  }
  const std::string rest = base.substr(schemeEnd + 3);
  if (rest.empty()) {
    throw std::runtime_error(
        "CloudEmbeddingProvider: embeddings.api_base has no host (got \"" +
        base + "\")");
  }
  const auto slash = rest.find('/');
  const std::string hostport =
      slash == std::string::npos ? rest : rest.substr(0, slash);
  const std::string pathPrefix =
      slash == std::string::npos ? std::string() : rest.substr(slash);
  if (hostport.empty()) {
    throw std::runtime_error(
        "CloudEmbeddingProvider: embeddings.api_base has no host (got \"" +
        base + "\")");
  }
  ParsedBase out;
  out.origin = scheme + "://" + hostport;
  out.embeddingsPath = pathPrefix + "/embeddings";
  out.normalized = base;
  return out;
}
}  // namespace

CloudEmbeddingProvider::CloudEmbeddingProvider(const std::string& apiKeyEnvVar,
                                               std::string apiBase,
                                               std::string model,
                                               std::size_t dimensions) {
  if (!apiKeyEnvVar.empty()) {
    const char* key = std::getenv(apiKeyEnvVar.c_str());
    if (key == nullptr || key[0] == '\0') {
      throw std::runtime_error(
          "CloudEmbeddingProvider: environment variable '" + apiKeyEnvVar +
          "' (named by embeddings.api_key_env) is not set");
    }
    apiKey_ = key;
  }

  const auto parsed = parseApiBase(std::move(apiBase));
  origin_ = parsed.origin;
  embeddingsPath_ = parsed.embeddingsPath;
  apiBase_ = parsed.normalized;
  model_ = model.empty() ? kDefaultModel : std::move(model);
  dimensions_ = dimensions == 0 ? kDefaultDimensions : dimensions;
}

std::vector<float> CloudEmbeddingProvider::embed(const std::string& text) {
  httplib::Client client(origin_);
  if (!apiKey_.empty()) {
    client.set_bearer_token_auth(apiKey_);
  }
  client.set_connection_timeout(10);
  client.set_read_timeout(30);

  nlohmann::json requestBody;
  requestBody["model"] = model_;
  requestBody["input"] = text;

  const auto res =
      client.Post(embeddingsPath_, requestBody.dump(), "application/json");
  if (!res) {
    throw std::runtime_error("CloudEmbeddingProvider::embed: HTTP request failed (" +
                              httplib::to_string(res.error()) + ")");
  }
  if (res->status != 200) {
    throw std::runtime_error("CloudEmbeddingProvider::embed: embeddings API returned HTTP " +
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

  if (result.size() != dimensions_) {
    throw std::runtime_error("CloudEmbeddingProvider::embed: expected " +
                              std::to_string(dimensions_) + " dimensions, got " +
                              std::to_string(result.size()) +
                              " (set embeddings.dimensions to match this model)");
  }

  return result;
}

std::size_t CloudEmbeddingProvider::dimensions() const { return dimensions_; }

std::string CloudEmbeddingProvider::modelIdentifier() const {
  return "cloud:" + apiBase_ + ":" + model_;
}

}  // namespace wikicore::embeddings
