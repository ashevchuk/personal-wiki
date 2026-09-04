#include "config/AppConfig.h"

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
  }
  if (auto* vault = root["vault"].as_table()) {
    cfg.vaultPath = (*vault)["path"].value_or(cfg.vaultPath);
  }
  if (auto* index = root["index"].as_table()) {
    cfg.dbPath = (*index)["db_path"].value_or(cfg.dbPath);
  }
  if (auto* mcp = root["mcp"].as_table()) {
    cfg.mcpScope = (*mcp)["scope"].value_or(cfg.mcpScope);
    cfg.mcpWriteAccess = (*mcp)["write_access"].value_or(cfg.mcpWriteAccess);
  }
  if (auto* attachments = root["attachments"].as_table()) {
    if (auto* mimeTypes = (*attachments)["mime_types"].as_table()) {
      for (const auto& [extKey, node] : *mimeTypes) {
        if (const auto mime = node.value<std::string>()) {
          cfg.attachmentMimeTypes[std::string(extKey.str())] = *mime;
        }
      }
    }
    if (auto* inlineSafe = (*attachments)["inline_safe_extensions"].as_array()) {
      for (const auto& node : *inlineSafe) {
        if (const auto ext = node.value<std::string>()) {
          cfg.attachmentInlineSafeExtensions.insert(*ext);
        }
      }
    }
  }
  if (auto* log = root["log"].as_table()) {
    cfg.logLevel = (*log)["level"].value_or(cfg.logLevel);
  }

  return cfg;
}

}  // namespace wikicore::config
