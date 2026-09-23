#pragma once

#include "index/Database.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wikicore::index {

struct AgentChatSummary {
  std::string id;
  std::string title;
  std::string createdAt;
  std::string updatedAt;
};

struct AgentChatRecord {
  std::string id;
  std::string title;
  std::string createdAt;
  std::string updatedAt;
  std::string eventsJson;
  std::string messagesJson;
};

// Persisted Chat panel history. Draft sessions stay in-RAM only.
// Same sqlite3 connection as the rest of the index; mutex guards
// multi-statement sequences the way IndexUpdater does.
class AgentChatStore {
 public:
  explicit AgentChatStore(Database& db) : db_(db) {}

  void upsert(const AgentChatRecord& row);
  std::optional<AgentChatRecord> get(const std::string& id) const;
  std::vector<AgentChatSummary> list(int limit = 50) const;
  void rename(const std::string& id, const std::string& title);
  void remove(const std::string& id);

 private:
  Database& db_;
  mutable std::mutex mu_;
};

}  // namespace wikicore::index
