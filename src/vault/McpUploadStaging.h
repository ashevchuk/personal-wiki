#pragma once

#include "vault/VaultRepository.h"

#include <optional>
#include <string>
#include <string_view>

namespace wikicore::vault {

struct McpUploadTicket {
  std::string id;
  std::string documentPath;
  std::string filename;
};

// One-shot tickets for a remote-MCP large-file upload. A ticket is a
// `.mcp-uploads/<uuid>.meta` file inside the vault (dotdir — skipped by
// IndexBuilder/VaultWatcher, same as `.uploads-tmp`). Knowing the UUID is
// what authorizes PUT /mcp/uploads/{id}: the follow-up HTTP request does
// not need the MCP bearer token, so an agent can `curl -T file.pdf` that
// URL without pasting a secret, and the file bytes never enter the
// model's context.
//
// Tickets expire after an hour; begin() also sweeps stale ones.
class McpUploadStaging {
 public:
  explicit McpUploadStaging(VaultRepository& vault) : vault_(vault) {}

  std::string begin(const std::string& documentPath, const std::string& filename);
  std::optional<McpUploadTicket> get(std::string_view id) const;
  void remove(std::string_view id);
  static bool isValidId(std::string_view id);

 private:
  void expireStale();
  static std::string metaRelative(std::string_view id);

  VaultRepository& vault_;
};

}  // namespace wikicore::vault
