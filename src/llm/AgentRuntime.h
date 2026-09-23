#pragma once

#include "index/AgentChatStore.h"
#include "index/FtsSearch.h"
#include "index/IndexUpdater.h"
#include "index/McpAuditLog.h"
#include "index/NavQueries.h"
#include "index/QueryBlocks.h"
#include "index/SnapshotStore.h"
#include "llm/ChatClient.h"
#include "vault/DocumentService.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace wikicore::llm {

// Wiki UI the human currently has open (Chat only). The server cannot
// see the browser URL; the client sends this on every chat turn.
struct AgentUiContext {
  std::string page;   // document | folder | search | graph | edit | history | account | other
  std::string path;   // vault-relative, for document/folder/edit/history
  std::string title;
};

struct AgentDocumentSnapshot {
  std::string path;
  std::string title;
  std::string type;
  std::string body;
  std::string selection;  // current editor selection, wiki-link form
  // Text before the caret when the selection is empty. nullopt means
  // the client did not capture a caret; empty string means the start.
  std::optional<std::string> caretBefore;
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
  std::string kind;  // draft | chat
  std::string title;
  std::string status;  // running | done | error | cancelled
  std::vector<AgentEvent> events;
  std::optional<AgentDraft> draft;
};

// In-memory agent sessions (editor Draft, or floating vault Chat).
// One ChatClient call sequence per user message, run on a private
// thread so Drogon's request threads are not held open for the cloud
// round-trip. Only one session may be `running` at a time (shared
// ChatClient). Draft write-tools fill the editor; Chat is read-only.
// Nothing is written to disk until the human hits Save.
class AgentRuntime {
 public:
  AgentRuntime(index::FtsSearch& search, vault::DocumentService& documents,
               index::NavQueries& nav, index::IndexUpdater& indexUpdater,
               index::McpAuditLog* auditLog, ChatClient* chat,
               std::string systemPrompt = {}, std::string chatSystemPrompt = {},
               index::AgentChatStore* chats = nullptr,
               index::QueryBlocks* queryBlocks = nullptr,
               index::SnapshotStore* snapshots = nullptr);
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

  // Vault Q&A — same client, read-only tools. ui is the page/folder
  // currently open in the browser (refreshed on every turn).
  std::string startChat(const std::string& instruction, AgentUiContext ui = {});
  void sendChat(const std::string& sessionId, const std::string& instruction,
                AgentUiContext ui = {});

  std::vector<index::AgentChatSummary> listChats() const;
  void renameChat(const std::string& sessionId, const std::string& title);
  // Hydrate a persisted chat into RAM if it is not already live.
  bool loadChat(const std::string& sessionId);

  std::optional<AgentSessionView> view(const std::string& sessionId) const;
  // Monotonic counter bumped on every event append/mutation. SSE waiters
  // sleep on waitGeneration until this differs from `seen`.
  std::uint64_t generation(const std::string& sessionId) const;
  bool waitGeneration(const std::string& sessionId, std::uint64_t seen,
                      std::chrono::milliseconds timeout) const;
  // Abort an in-flight cloud call. Keeps the session so the log stays
  // visible; a later send() starts a new turn. Throws if missing.
  void cancel(const std::string& sessionId);
  void drop(const std::string& sessionId);

 private:
  struct Session;

  void runLoop(const std::string& sessionId);
  std::string executeTool(Session& session, const std::string& name,
                          const nlohmann::json& args);
  void appendEventLocked(Session& session, AgentEvent ev);
  void appendDeltaLocked(Session& session, std::string_view chunk);
  void finishStreamingLocked(Session& session);
  void persistChat(const Session& session);
  std::shared_ptr<Session> sessionFromRecord(const index::AgentChatRecord& row) const;
  static nlohmann::json toolSchemas(const std::string& kind);

  index::FtsSearch& search_;
  vault::DocumentService& documents_;
  index::NavQueries& nav_;
  index::IndexUpdater& indexUpdater_;
  index::McpAuditLog* auditLog_ = nullptr;
  ChatClient* chat_ = nullptr;
  index::AgentChatStore* chatStore_ = nullptr;
  index::QueryBlocks* queryBlocks_ = nullptr;
  index::SnapshotStore* snapshots_ = nullptr;
  std::string systemPrompt_;
  std::string chatSystemPrompt_;

  mutable std::mutex mu_;
  mutable std::condition_variable cv_;
  std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
};

}  // namespace wikicore::llm
