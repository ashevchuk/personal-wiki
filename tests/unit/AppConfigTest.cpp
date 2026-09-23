#include "config/AppConfig.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace wikicore::config;

namespace {

// Writes `contents` to a fresh temp file and returns its path — removed on
// destruction so a failed assertion doesn't leak scratch files across runs.
class TempConfigFile {
 public:
  explicit TempConfigFile(const std::string& contents)
      : path_(fs::temp_directory_path() /
              fs::path("wiki-appconfig-test-" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                        ".toml")) {
    std::ofstream(path_) << contents;
  }
  ~TempConfigFile() { fs::remove(path_); }
  TempConfigFile(const TempConfigFile&) = delete;
  TempConfigFile& operator=(const TempConfigFile&) = delete;
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

}  // namespace

TEST_CASE("AppConfig::load: base_path defaults to empty when config.toml has no "
          "[server] table at all",
          "[AppConfig]") {
  TempConfigFile file("[vault]\npath = \"./vault_data\"\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.basePath.empty());
}

TEST_CASE("AppConfig::load: base_path is read verbatim when it has no trailing slash",
          "[AppConfig]") {
  TempConfigFile file("[server]\nbase_path = \"/wiki\"\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.basePath == "/wiki");
}

TEST_CASE("AppConfig::load: a trailing slash on base_path is stripped — "
          "PageRoutes.cpp always appends its own",
          "[AppConfig]") {
  TempConfigFile file("[server]\nbase_path = \"/wiki/\"\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.basePath == "/wiki");
}

TEST_CASE("AppConfig::load: repeated trailing slashes are all stripped",
          "[AppConfig]") {
  TempConfigFile file("[server]\nbase_path = \"/wiki///\"\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.basePath == "/wiki");
}

TEST_CASE("AppConfig::load: a bare \"/\" collapses to empty — same meaning as "
          "\"no prefix\", not a one-character prefix",
          "[AppConfig]") {
  TempConfigFile file("[server]\nbase_path = \"/\"\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.basePath.empty());
}

TEST_CASE("AppConfig::load: theme defaults to empty when unset — the client "
          "picks its own hardcoded fallback in that case",
          "[AppConfig]") {
  TempConfigFile file("[server]\nbase_path = \"/wiki\"\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.defaultTheme.empty());
}

TEST_CASE("AppConfig::load: theme is read verbatim, no allowlist check at "
          "this layer — shell.html's bootstrap script validates it instead",
          "[AppConfig]") {
  TempConfigFile file("[server]\ntheme = \"classic\"\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.defaultTheme == "classic");
}

TEST_CASE("AppConfig::load: an unrecognized theme name is still read verbatim "
          "— deliberately not this layer's job to reject it",
          "[AppConfig]") {
  TempConfigFile file("[server]\ntheme = \"nonexistent-theme\"\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.defaultTheme == "nonexistent-theme");
}

TEST_CASE("AppConfig::load: embeddings.provider defaults to \"none\" when "
          "config.toml has no [embeddings] table at all",
          "[AppConfig]") {
  TempConfigFile file("[vault]\npath = \"./vault_data\"\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.embeddingsProvider == "none");
  REQUIRE(cfg.embeddingsModelPath.empty());
  REQUIRE(cfg.embeddingsApiKeyEnv.empty());
}

TEST_CASE("AppConfig::load: embeddings.provider/model_path are read verbatim",
          "[AppConfig]") {
  TempConfigFile file(
      "[embeddings]\nprovider = \"local\"\nmodel_path = \"/opt/wiki/models/m.gguf\"\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.embeddingsProvider == "local");
  REQUIRE(cfg.embeddingsModelPath == "/opt/wiki/models/m.gguf");
}

TEST_CASE("AppConfig::load: embeddings.api_key_env is read verbatim as a name, "
          "not resolved to the environment variable's own value",
          "[AppConfig]") {
  TempConfigFile file("[embeddings]\nprovider = \"cloud\"\napi_key_env = \"WIKI_EMBEDDINGS_API_KEY\"\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.embeddingsProvider == "cloud");
  REQUIRE(cfg.embeddingsApiKeyEnv == "WIKI_EMBEDDINGS_API_KEY");
}

TEST_CASE("AppConfig::load: embeddings.api_base/model/dimensions are read for "
          "an OpenAI-compatible cloud endpoint",
          "[AppConfig]") {
  TempConfigFile file(
      "[embeddings]\nprovider = \"cloud\"\n"
      "api_base = \"http://127.0.0.1:11434/v1\"\n"
      "model = \"nomic-embed-text\"\n"
      "dimensions = 768\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.embeddingsApiBase == "http://127.0.0.1:11434/v1");
  REQUIRE(cfg.embeddingsCloudModel == "nomic-embed-text");
  REQUIRE(cfg.embeddingsCloudDimensions == 768);
}

TEST_CASE("AppConfig::load: embeddings.api_base/model/dimensions default empty/"
          "zero when unset (CloudEmbeddingProvider fills OpenAI defaults)",
          "[AppConfig]") {
  TempConfigFile file("[embeddings]\nprovider = \"cloud\"\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.embeddingsApiBase.empty());
  REQUIRE(cfg.embeddingsCloudModel.empty());
  REQUIRE(cfg.embeddingsCloudDimensions == 0);
}

TEST_CASE("AppConfig::load: a negative embeddings.dimensions is rejected",
          "[AppConfig]") {
  TempConfigFile file("[embeddings]\nprovider = \"cloud\"\ndimensions = -1\n");
  REQUIRE_THROWS_AS(AppConfig::load(file.path().string()), std::runtime_error);
}

TEST_CASE("AppConfig::load: embeddings.max_distance/min_content_words/"
          "semantic_top_k default when not set in config.toml",
          "[AppConfig]") {
  TempConfigFile file("[vault]\npath = \"./vault_data\"\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.embeddingsMaxDistance == 0.5);
  REQUIRE(cfg.embeddingsMinContentWords == 6);
  REQUIRE(cfg.embeddingsSemanticTopK == 5);
  REQUIRE(cfg.embeddingsQueryPrefix.empty());
}

TEST_CASE("AppConfig::load: embeddings.query_prefix/max_distance/"
          "min_content_words/semantic_top_k are read verbatim when set",
          "[AppConfig]") {
  TempConfigFile file(
      "[embeddings]\nprovider = \"local\"\n"
      "query_prefix = \"Represent this sentence for searching relevant passages: \"\n"
      "max_distance = 0.42\n"
      "min_content_words = 10\n"
      "semantic_top_k = 3\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.embeddingsQueryPrefix ==
          "Represent this sentence for searching relevant passages: ");
  REQUIRE(cfg.embeddingsMaxDistance == 0.42);
  REQUIRE(cfg.embeddingsMinContentWords == 10);
  REQUIRE(cfg.embeddingsSemanticTopK == 3);
}

TEST_CASE("AppConfig::load: llm.provider defaults to none when unset",
          "[AppConfig]") {
  TempConfigFile file("[vault]\npath = \"./vault_data\"\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.llmProvider == "none");
  REQUIRE(cfg.llmApiKeyEnv.empty());
  REQUIRE(cfg.llmApiBase.empty());
  REQUIRE(cfg.llmModel.empty());
  REQUIRE(cfg.llmSystemPrompt.empty());
  REQUIRE(cfg.llmChatSystemPrompt.empty());
}

TEST_CASE("AppConfig::load: llm cloud knobs are read as names, not env values",
          "[AppConfig]") {
  TempConfigFile file(
      "[llm]\nprovider = \"cloud\"\n"
      "api_key_env = \"ANTHROPIC_API_KEY\"\n"
      "api_base = \"https://api.anthropic.com/v1\"\n"
      "model = \"claude-sonnet-4-6\"\n");
  const AppConfig cfg = AppConfig::load(file.path().string());
  REQUIRE(cfg.llmProvider == "cloud");
  REQUIRE(cfg.llmApiKeyEnv == "ANTHROPIC_API_KEY");
  REQUIRE(cfg.llmApiBase == "https://api.anthropic.com/v1");
  REQUIRE(cfg.llmModel == "claude-sonnet-4-6");
  REQUIRE(cfg.llmSystemPrompt.empty());
  REQUIRE(cfg.llmChatSystemPrompt.empty());
}

TEST_CASE("AppConfig::load: llm.system_prompt is optional and read verbatim",
          "[AppConfig]") {
  TempConfigFile missing("[llm]\nprovider = \"cloud\"\n");
  REQUIRE(AppConfig::load(missing.path().string()).llmSystemPrompt.empty());

  TempConfigFile empty("[llm]\nsystem_prompt = \"\"\n");
  REQUIRE(AppConfig::load(empty.path().string()).llmSystemPrompt.empty());

  TempConfigFile set("[llm]\nsystem_prompt = \"Be terse.\\nKeep wiki-links.\"\n");
  REQUIRE(AppConfig::load(set.path().string()).llmSystemPrompt ==
          "Be terse.\nKeep wiki-links.");

  TempConfigFile multiline(
      "[llm]\nsystem_prompt = \"\"\"\nLine one\nLine two\n\"\"\"\n");
  REQUIRE(AppConfig::load(multiline.path().string()).llmSystemPrompt ==
          "Line one\nLine two\n");
}

TEST_CASE("AppConfig::load: llm.chat_system_prompt is optional and read verbatim",
          "[AppConfig]") {
  TempConfigFile missing("[llm]\nprovider = \"cloud\"\n");
  REQUIRE(AppConfig::load(missing.path().string()).llmChatSystemPrompt.empty());

  TempConfigFile empty("[llm]\nchat_system_prompt = \"\"\n");
  REQUIRE(AppConfig::load(empty.path().string()).llmChatSystemPrompt.empty());

  TempConfigFile set("[llm]\nchat_system_prompt = \"Answer from the vault.\"\n");
  REQUIRE(AppConfig::load(set.path().string()).llmChatSystemPrompt ==
          "Answer from the vault.");

  TempConfigFile multiline(
      "[llm]\nchat_system_prompt = \"\"\"\nChat one\nChat two\n\"\"\"\n");
  REQUIRE(AppConfig::load(multiline.path().string()).llmChatSystemPrompt ==
          "Chat one\nChat two\n");
}
