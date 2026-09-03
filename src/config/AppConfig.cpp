#include "config/AppConfig.h"

#include "util/BasePath.h"

#include <toml++/toml.h>

#include <filesystem>
#include <stdexcept>

namespace wikicore::config {

AppConfig AppConfig::load(const std::string& path) {
  AppConfig cfg;

  if (!std::filesystem::exists(path)) {
    return cfg;  // defaults are a valid config (fresh checkout, tests, ...)
  }

  toml::table root;
  try {
    root = toml::parse_file(path);
  } catch (const toml::parse_error& err) {
    throw std::runtime_error("failed to parse config '" + path +
                              "': " + std::string(err.description()));
  }

  if (auto* server = root["server"].as_table()) {
    cfg.listenAddr = (*server)["listen_addr"].value_or(cfg.listenAddr);
    cfg.port = static_cast<uint16_t>(
        (*server)["port"].value_or(static_cast<int64_t>(cfg.port)));
    cfg.threads = static_cast<size_t>(
        (*server)["threads"].value_or(static_cast<int64_t>(cfg.threads)));
    cfg.basePath = util::normalizeBasePath((*server)["base_path"].value_or(std::string()));
  }
  if (auto* vault = root["vault"].as_table()) {
    cfg.vaultPath = (*vault)["path"].value_or(cfg.vaultPath);
  }
  if (auto* index = root["index"].as_table()) {
    cfg.dbPath = (*index)["db_path"].value_or(cfg.dbPath);
  }
  if (auto* mcp = root["mcp"].as_table()) {
    cfg.mcpScope = (*mcp)["scope"].value_or(cfg.mcpScope);
  }
  if (auto* log = root["log"].as_table()) {
    cfg.logLevel = (*log)["level"].value_or(cfg.logLevel);
  }

  return cfg;
}

}  // namespace wikicore::config
