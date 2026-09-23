#include "index/AgentChatStore.h"

#include "index/Statement.h"
#include "util/Time.h"

#include <sqlite3.h>

#include <cstdint>
#include <stdexcept>

namespace wikicore::index {

void AgentChatStore::upsert(const AgentChatRecord& row) {
  if (row.id.empty()) throw std::runtime_error("chat id is required");
  std::lock_guard<std::mutex> lock(mu_);
  Statement stmt(db_.handle(),
                 "INSERT INTO agent_chats(id, title, created_at, updated_at, "
                 "events_json, messages_json) VALUES (?1, ?2, ?3, ?4, ?5, ?6) "
                 "ON CONFLICT(id) DO UPDATE SET "
                 "title = excluded.title, "
                 "updated_at = excluded.updated_at, "
                 "events_json = excluded.events_json, "
                 "messages_json = excluded.messages_json;");
  stmt.bind(1, row.id)
      .bind(2, row.title)
      .bind(3, row.createdAt)
      .bind(4, row.updatedAt)
      .bind(5, row.eventsJson)
      .bind(6, row.messagesJson);
  stmt.run();
}

std::optional<AgentChatRecord> AgentChatStore::get(const std::string& id) const {
  std::lock_guard<std::mutex> lock(mu_);
  Statement stmt(db_.handle(),
                 "SELECT id, title, created_at, updated_at, events_json, messages_json "
                 "FROM agent_chats WHERE id = ?1;");
  stmt.bind(1, id);
  if (!stmt.step()) return std::nullopt;
  AgentChatRecord row;
  row.id = stmt.columnText(0);
  row.title = stmt.columnText(1);
  row.createdAt = stmt.columnText(2);
  row.updatedAt = stmt.columnText(3);
  row.eventsJson = stmt.columnText(4);
  row.messagesJson = stmt.columnText(5);
  return row;
}

std::vector<AgentChatSummary> AgentChatStore::list(int limit) const {
  if (limit < 1) limit = 1;
  if (limit > 200) limit = 200;
  std::lock_guard<std::mutex> lock(mu_);
  Statement stmt(db_.handle(),
                 "SELECT id, title, created_at, updated_at FROM agent_chats "
                 "ORDER BY updated_at DESC, rowid DESC LIMIT ?1;");
  stmt.bind(1, static_cast<int64_t>(limit));
  std::vector<AgentChatSummary> out;
  while (stmt.step()) {
    out.push_back(AgentChatSummary{
        stmt.columnText(0),
        stmt.columnText(1),
        stmt.columnText(2),
        stmt.columnText(3),
    });
  }
  return out;
}

void AgentChatStore::rename(const std::string& id, const std::string& title) {
  if (id.empty()) throw std::runtime_error("chat id is required");
  if (title.empty()) throw std::runtime_error("title is required");
  std::lock_guard<std::mutex> lock(mu_);
  Statement stmt(db_.handle(),
                 "UPDATE agent_chats SET title = ?1, updated_at = ?2 WHERE id = ?3;");
  stmt.bind(1, title).bind(2, util::nowIso8601()).bind(3, id);
  stmt.run();
  if (sqlite3_changes(db_.handle()) == 0) {
    throw std::runtime_error("session not found");
  }
}

void AgentChatStore::remove(const std::string& id) {
  if (id.empty()) return;
  std::lock_guard<std::mutex> lock(mu_);
  Statement stmt(db_.handle(), "DELETE FROM agent_chats WHERE id = ?1;");
  stmt.bind(1, id);
  stmt.run();
}

}  // namespace wikicore::index
