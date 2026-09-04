#pragma once

#include "index/Database.h"

#include <string>
#include <vector>

namespace wikicore::index {

struct McpAuditEntry {
  int64_t id;
  std::string at;
  std::string toolName;
  std::string path;
  bool success;
  std::string detail;  // error message on failure; a short summary on success
};

// Records every call to an MCP write tool (create_document/
// update_document — see McpServer.cpp), success or failure alike. This
// is the accountability half of [mcp].write_access: the flag lets an
// LLM write to the vault unsupervised, this table is what lets the
// human admin find out what it actually did, after the fact, via
// GET /api/admin/mcp-audit-log (AdminRoutes.cpp).
class McpAuditLog {
 public:
  explicit McpAuditLog(Database& db) : db_(db) {}

  void record(const std::string& toolName, const std::string& path, bool success,
              const std::string& detail);

  // Newest first.
  std::vector<McpAuditEntry> listRecent(int limit) const;

 private:
  Database& db_;
};

}  // namespace wikicore::index
