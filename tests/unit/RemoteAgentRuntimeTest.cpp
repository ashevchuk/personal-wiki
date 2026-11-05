#include "index/CalendarQueries.h"
#include "index/Database.h"
#include "index/FtsSearch.h"
#include "index/IndexUpdater.h"
#include "index/McpAuditLog.h"
#include "index/NavQueries.h"
#include "llm/RemoteAgentRuntime.h"
#include "vault/DocumentService.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace fs = std::filesystem;
using namespace wikicore;

namespace {

class TempEnv {
 public:
  TempEnv()
      : root_(fs::temp_directory_path() /
              fs::path("wiki-remote-agent-test-" +
                        std::to_string(reinterpret_cast<std::uintptr_t>(this)))) {
    fs::remove_all(root_);
    fs::create_directories(root_ / "vault");
  }
  ~TempEnv() { fs::remove_all(root_); }
  TempEnv(const TempEnv&) = delete;
  TempEnv& operator=(const TempEnv&) = delete;
  fs::path vaultRoot() const { return root_ / "vault"; }
  fs::path dbPath() const { return root_ / "index.db"; }

 private:
  fs::path root_;
};

class ScriptedChatClient : public llm::ChatClient {
 public:
  std::vector<llm::ChatCompletion> replies;
  std::size_t index = 0;

  llm::ChatCompletion complete(const std::vector<llm::ChatMessage>&, const nlohmann::json&,
                               const llm::ChatDeltaFn&) override {
    if (index >= replies.size()) {
      llm::ChatCompletion done;
      done.content = "done";
      return done;
    }
    return replies[index++];
  }
};

llm::ChatCompletion toolCall(const std::string& name, const std::string& args) {
  llm::ChatCompletion c;
  llm::ToolCall call;
  call.id = "call-" + name;
  call.name = name;
  call.arguments = args;
  c.toolCalls.push_back(call);
  return c;
}

llm::ChatCompletion textReply(const std::string& text) {
  llm::ChatCompletion c;
  c.content = text;
  return c;
}

struct Fixture {
  TempEnv env;
  index::Database db{env.dbPath()};
  index::IndexUpdater indexUpdater{db};
  index::SnapshotStore snapshots{db};
  vault::VaultRepository repo{env.vaultRoot()};
  vault::DocumentService documents{repo, indexUpdater, snapshots};
  index::FtsSearch search{db};
  index::NavQueries nav{db};
  index::CalendarQueries calendar{db};
  index::McpAuditLog auditLog{db};

  Fixture() { db.migrate(); }
};

}  // namespace

TEST_CASE("RemoteAgentRuntime: create_document on the first tool call, audited "
          "with the token's label",
          "[RemoteAgentRuntime]") {
  Fixture f;
  ScriptedChatClient chat;
  chat.replies.push_back(toolCall(
      "create_document",
      R"({"path":"notes/a.md","title":"A","body":"hello","visibility":"private"})"));

  llm::RemoteAgentRuntime agent(chat, f.documents, f.search, f.nav, f.calendar, f.indexUpdater,
                                f.auditLog, "");
  const auto turn =
      agent.run({llm::ChatMessage{"user", "create a note called A", "", {}}}, "tray-app");

  REQUIRE(turn.documentsCreated == std::vector<std::string>{"notes/a.md"});
  REQUIRE(turn.reply == "Created notes/a.md");

  const auto entries = f.auditLog.listRecent(10);
  REQUIRE(entries.size() == 1);
  REQUIRE(entries[0].toolName == "remote-agent:create_document");
  REQUIRE(entries[0].success);
  REQUIRE(entries[0].tokenLabel == "tray-app");
}

TEST_CASE("RemoteAgentRuntime: a path collision is fed back to the model, which "
          "retries at a different path instead of failing the turn",
          "[RemoteAgentRuntime]") {
  Fixture f;
  vault::DocumentInput existing;
  existing.title = "Existing";
  existing.body = "already here";
  f.documents.create("notes/a.md", existing);

  ScriptedChatClient chat;
  chat.replies.push_back(
      toolCall("create_document", R"({"path":"notes/a.md","title":"A","body":"x"})"));
  chat.replies.push_back(
      toolCall("create_document", R"({"path":"notes/a-2.md","title":"A","body":"x"})"));

  llm::RemoteAgentRuntime agent(chat, f.documents, f.search, f.nav, f.calendar, f.indexUpdater,
                                f.auditLog, "");
  const auto turn = agent.run({llm::ChatMessage{"user", "create A", "", {}}}, "tray-app");

  REQUIRE(turn.documentsCreated == std::vector<std::string>{"notes/a-2.md"});

  const auto entries = f.auditLog.listRecent(10);
  REQUIRE(entries.size() == 2);
  // Newest first: the successful retry, then the original collision.
  REQUIRE(entries[0].success);
  REQUIRE_FALSE(entries[1].success);
}

TEST_CASE("RemoteAgentRuntime: search_documents results reach the model with "
          "plain markdown emphasis, not raw FTS5 match-marker bytes",
          "[RemoteAgentRuntime]") {
  Fixture f;
  vault::DocumentInput existing;
  existing.title = "Smart Pointers Cheat Sheet";
  existing.visibility = "private";
  existing.tags = {"lang/cpp"};
  existing.body = "unique_ptr owns a value; shared_ptr shares it with others.\n";
  f.documents.create("notes/smart-pointers.md", existing);

  ScriptedChatClient chat;
  chat.replies.push_back(toolCall("search_documents", R"({"query":"smart pointers"})"));
  chat.replies.push_back(textReply("You already have notes/smart-pointers.md for that."));

  llm::RemoteAgentRuntime agent(chat, f.documents, f.search, f.nav, f.calendar, f.indexUpdater,
                                f.auditLog, "");
  const auto turn =
      agent.run({llm::ChatMessage{"user", "create a smart pointers note", "", {}}}, "tray-app");

  REQUIRE(turn.documentsCreated.empty());
  REQUIRE(turn.reply == "You already have notes/smart-pointers.md for that.");

  const auto entries = f.auditLog.listRecent(10);
  REQUIRE(entries.size() == 1);
  REQUIRE(entries[0].toolName == "remote-agent:search_documents");
  for (char c : entries[0].detail) {
    REQUIRE(static_cast<unsigned char>(c) != 0x01);
    REQUIRE(static_cast<unsigned char>(c) != 0x02);
  }
}

TEST_CASE("RemoteAgentRuntime: get_document on a real document, and on a miss",
          "[RemoteAgentRuntime]") {
  Fixture f;
  vault::DocumentInput existing;
  existing.title = "Existing";
  existing.body = "body text";
  f.documents.create("notes/a.md", existing);

  ScriptedChatClient chat;
  chat.replies.push_back(toolCall("get_document", R"({"id_or_path":"notes/a.md"})"));
  chat.replies.push_back(toolCall("get_document", R"({"id_or_path":"notes/missing.md"})"));
  chat.replies.push_back(textReply("ok"));

  llm::RemoteAgentRuntime agent(chat, f.documents, f.search, f.nav, f.calendar, f.indexUpdater,
                                f.auditLog, "");
  const auto turn = agent.run({llm::ChatMessage{"user", "what's in notes/a.md?", "", {}}},
                               "tray-app");

  REQUIRE(turn.reply == "ok");
  const auto entries = f.auditLog.listRecent(10);
  REQUIRE(entries.size() == 2);
  // Newest first: the miss, then the hit.
  REQUIRE_FALSE(entries[0].success);
  REQUIRE(entries[1].success);
  REQUIRE(entries[1].path == "notes/a.md");
}

TEST_CASE("RemoteAgentRuntime: list_tags and an unknown tool name are both "
          "handled without throwing",
          "[RemoteAgentRuntime]") {
  Fixture f;
  ScriptedChatClient chat;
  chat.replies.push_back(toolCall("list_tags", "{}"));
  chat.replies.push_back(toolCall("update_document", R"({"path":"x"})"));
  chat.replies.push_back(textReply("done"));

  llm::RemoteAgentRuntime agent(chat, f.documents, f.search, f.nav, f.calendar, f.indexUpdater,
                                f.auditLog, "");
  const auto turn = agent.run({llm::ChatMessage{"user", "what tags exist?", "", {}}}, "tray-app");

  REQUIRE(turn.reply == "done");
  REQUIRE(turn.documentsCreated.empty());
}

TEST_CASE("RemoteAgentRuntime: hitting the step limit without ever writing "
          "returns a fallback reply instead of hanging or throwing",
          "[RemoteAgentRuntime]") {
  Fixture f;
  ScriptedChatClient chat;
  for (int i = 0; i < 10; ++i) {
    chat.replies.push_back(toolCall("list_tags", "{}"));
  }

  llm::RemoteAgentRuntime agent(chat, f.documents, f.search, f.nav, f.calendar, f.indexUpdater,
                                f.auditLog, "");
  const auto turn = agent.run({llm::ChatMessage{"user", "loop forever", "", {}}}, "tray-app");

  REQUIRE(turn.documentsCreated.empty());
  REQUIRE_FALSE(turn.reply.empty());
}
