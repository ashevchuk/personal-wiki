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
// update_document/attach_file — see McpServer.cpp), success or failure alike. This
// is the accountability half of [mcp].write_access: the flag lets an
// LLM write to the vault unsupervised, this table is what lets the
// human admin find out what it actually did, after the fact, via
// GET /api/admin/mcp-audit-log (AdminRoutes.cpp).
//
// Also reused (with an "admin:"-prefixed toolName, same convention as
// RemoteMcpRoutes.cpp's "remote:" prefix for the HTTP MCP transport) for
// admin-triggered actions that aren't MCP tool calls at all but are the
// same category of question — "what happened to this vault, and when":
// POST /api/admin/embeddings-status/reembed[-all] (AdminRoutes.cpp).
// One shared log/view for that question beats a second, parallel one.
class McpAuditLog {
 public:
  explicit McpAuditLog(Database& db) : db_(db) {}

  // Found live: GET /api/admin/mcp-audit-log only ever shows the 200
  // most recent rows TOTAL, shared across every category (remote MCP,
  // admin re-embeds, Draft/Chat) before the Web UI splits that one
  // batch by tool_name prefix -- a session with real Chat/Draft use
  // (several tool calls per message) ages older remote-MCP/admin
  // entries out of that shared window within days, while this table
  // kept every row ever written, forever, with no retention at all.
  // 2000 is ten times that display window -- generous headroom for a
  // personal, single-admin wiki's actual traffic, while still bounding
  // the table instead of letting it grow for the life of the install.
  // record() prunes down to this on every call (same prune-on-write
  // shape as SessionStore::create()'s own pruneExpired()).
  static constexpr int64_t kMaxRows = 2000;

  void record(const std::string& toolName, const std::string& path, bool success,
              const std::string& detail);

  // Newest first.
  std::vector<McpAuditEntry> listRecent(int limit) const;

 private:
  Database& db_;
};

}  // namespace wikicore::index
