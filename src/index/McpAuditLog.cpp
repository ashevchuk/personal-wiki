#include "index/McpAuditLog.h"

#include "index/Statement.h"
#include "util/Time.h"

namespace wikicore::index {

void McpAuditLog::record(const std::string& toolName, const std::string& path, bool success,
                          const std::string& detail) {
  Statement insert(db_.handle(),
                    "INSERT INTO mcp_audit_log(at, tool_name, path, success, detail) "
                    "VALUES (?1, ?2, ?3, ?4, ?5);");
  insert.bind(1, util::nowIso8601())
      .bind(2, toolName)
      .bind(3, path)
      .bind(4, static_cast<int64_t>(success ? 1 : 0))
      .bind(5, detail);
  insert.run();
}

std::vector<McpAuditEntry> McpAuditLog::listRecent(int limit) const {
  Statement stmt(db_.handle(),
                  "SELECT id, at, tool_name, path, success, detail FROM mcp_audit_log "
                  "ORDER BY id DESC LIMIT ?1;");
  stmt.bind(1, static_cast<int64_t>(limit));

  std::vector<McpAuditEntry> results;
  while (stmt.step()) {
    results.push_back(McpAuditEntry{
        stmt.columnInt64(0),
        stmt.columnText(1),
        stmt.columnText(2),
        stmt.columnText(3),
        stmt.columnInt64(4) != 0,
        stmt.columnText(5),
    });
  }
  return results;
}

}  // namespace wikicore::index
