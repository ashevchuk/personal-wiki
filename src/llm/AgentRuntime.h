#pragma once

#include "index/FtsSearch.h"
#include "index/IndexUpdater.h"
#include "index/McpAuditLog.h"
#include "index/NavQueries.h"
#include "llm/ChatClient.h"
#include "vault/DocumentService.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace wikicore::llm {

struct AgentDocumentSnapshot {
  std::string path;
  std::string title;
  std::string type;
  std::string body;
  std::vector<std::string> tags;
  bool isNew = true;
};

struct AgentDraft {
  std::string path;
  std::string title;
  std::string type;
  std::string body;
  std::vector<std::string> tags;
};

struct AgentEvent {
  std::string type;  // user | assistant | tool | draft | error | done
  nlohmann::json data;
};

struct AgentSessionView {
  std::string id;
  std::string status;  // running | done | error
  std::vector<AgentEvent> events;
  std::optional<AgentDraft> draft;
};

// In-memory drafting sessions. One ChatClient call sequence per user
// message, run on a private thread so Drogon's request threads are not
// held open for the cloud round-trip. Read-only vault tools only —
// propose_draft fills the editor; nothing is written to disk until the
// human hits Save.
class AgentRuntime {
 public:
  AgentRuntime(index::FtsSearch& search, vault::DocumentService& documents,
               index::NavQueries& nav, index::IndexUpdater& indexUpdater,
               index::McpAuditLog* auditLog, ChatClient* chat,
               std::string systemPrompt = {});
  ~AgentRuntime();

  AgentRuntime(const AgentRuntime&) = delete;
  AgentRuntime& operator=(const AgentRuntime&) = delete;

  bool enabled() const { return chat_ != nullptr; }

  // Starts a new session and the first run. Throws runtime_error on
  // conflict (another session already running) or if disabled.
  std::string start(const std::string& instruction, AgentDocumentSnapshot snapshot);

  // Follow-up on an existing session with a fresh editor snapshot.
  void send(const std::string& sessionId, const std::string& instruction,
            AgentDocumentSnapshot snapshot);

  std::optional<AgentSessionView> view(const std::string& sessionId) const;
  void drop(const std::string& sessionId);

 private:
  struct Session;

  void runLoop(const std::string& sessionId);
  std::string executeTool(Session& session, const std::string& name,
                          const nlohmann::json& args);
  static nlohmann::json toolSchemas();

  index::FtsSearch& search_;
  vault::DocumentService& documents_;
  index::NavQueries& nav_;
  index::IndexUpdater& indexUpdater_;
  index::McpAuditLog* auditLog_ = nullptr;
  ChatClient* chat_ = nullptr;
  std::string systemPrompt_;

  mutable std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
};

}  // namespace wikicore::llm
