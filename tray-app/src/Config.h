#pragma once

#include <string>

namespace wikitray {

struct Config {
  std::string serverUrl = "https://home.enslaver.net/wiki";
  std::string token;
  std::string hotkey = "Ctrl+Alt+W";

  // Loads ~/.config/wiki-tray/config.toml, creating it with defaults (and
  // an empty token) on first run. Throws on a missing token, a config file
  // whose permissions aren't exactly 0600, or a parse error — this app
  // never starts with an unusable or insecurely-stored config.
  static Config load();
};

}  // namespace wikitray
