#include "Config.h"

#include <toml++/toml.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>

namespace wikitray {

namespace {

std::filesystem::path configDir() {
  const char* home = std::getenv("HOME");
  if (!home || *home == '\0') {
    throw std::runtime_error("Config::load: $HOME is not set");
  }
  return std::filesystem::path(home) / ".config" / "wiki-tray";
}

void writeDefaultConfig(const std::filesystem::path& path) {
  std::ofstream out(path);
  out << "[server]\n"
         "url = \"https://wiki.example.com/wiki\"\n"
         "\n"
         "[auth]\n"
         "token = \"\"\n"
         "\n"
         "[hotkey]\n"
         "binding = \"Ctrl+Alt+W\"\n";
  out.close();
  if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
    throw std::runtime_error("Config::load: failed to chmod 0600 '" +
                              path.string() + "'");
  }
}

void requireMode0600(const std::filesystem::path& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) {
    throw std::runtime_error("Config::load: failed to stat '" +
                              path.string() + "'");
  }
  const mode_t perms = st.st_mode & 0777;
  if (perms != (S_IRUSR | S_IWUSR)) {
    throw std::runtime_error("Config::load: '" + path.string() +
                              "' must be mode 0600 (it holds a bearer "
                              "token) — run: chmod 600 " +
                              path.string());
  }
}

}  // namespace

Config Config::load() {
  const auto dir = configDir();
  const auto path = dir / "config.toml";

  if (!std::filesystem::exists(path)) {
    std::filesystem::create_directories(dir);
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all);
    writeDefaultConfig(path);
    throw std::runtime_error("Config::load: wrote a fresh config to '" +
                              path.string() +
                              "' — fill in [auth].token and restart");
  }

  requireMode0600(path);

  toml::table root;
  try {
    root = toml::parse_file(path.string());
  } catch (const toml::parse_error& err) {
    throw std::runtime_error("Config::load: failed to parse '" +
                              path.string() + "': " +
                              std::string(err.description()));
  }

  Config cfg;
  if (auto* server = root["server"].as_table()) {
    cfg.serverUrl = (*server)["url"].value_or(cfg.serverUrl);
  }
  if (auto* auth = root["auth"].as_table()) {
    cfg.token = (*auth)["token"].value_or(cfg.token);
  }
  if (auto* hotkey = root["hotkey"].as_table()) {
    cfg.hotkey = (*hotkey)["binding"].value_or(cfg.hotkey);
  }

  if (cfg.token.empty()) {
    throw std::runtime_error("Config::load: '" + path.string() +
                              "' has an empty [auth].token");
  }

  return cfg;
}

}  // namespace wikitray
