#include "index/Database.h"
#include "index/FtsSearch.h"
#include "index/IndexUpdater.h"
#include "index/NavQueries.h"
#include "index/SnapshotStore.h"
#include "llm/AgentRuntime.h"
#include "vault/DocumentService.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;
using namespace wikicore;

namespace {

class TempEnv {
 public:
  TempEnv()
      : root_(fs::temp_directory_path() /
              fs::path("wiki-agent-test-" +
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
  std::string lastSystem;

  llm::ChatCompletion complete(const std::vector<llm::ChatMessage>& messages,
                               const nlohmann::json&) override {
    for (const auto& msg : messages) {
      if (msg.role == "system") {
        lastSystem = msg.content;
        break;
      }
    }
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

llm::AgentSessionView waitDone(llm::AgentRuntime& agent, const std::string& id) {
  for (int i = 0; i < 100; ++i) {
    auto view = agent.view(id);
    REQUIRE(view);
    if (view->status != "running") return *view;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  FAIL("agent session did not finish");
  return {};
}

}  // namespace

TEST_CASE("AgentRuntime: disabled start throws", "[AgentRuntime]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService documents(repo, indexUpdater, snapshots);
  index::FtsSearch search(db);
  index::NavQueries nav(db);
  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, nullptr);
  REQUIRE_FALSE(agent.enabled());
  llm::AgentDocumentSnapshot snap;
  REQUIRE_THROWS_AS(agent.start("write a note", snap), std::runtime_error);
}

TEST_CASE("AgentRuntime: search then propose_draft fills a draft without writing",
          "[AgentRuntime]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService documents(repo, indexUpdater, snapshots);

  vault::DocumentInput existing;
  existing.title = "Smart Pointers Cheat Sheet";
  existing.visibility = "private";
  existing.type = "note";
  existing.tags = {"lang/cpp"};
  existing.body = "unique_ptr owns; shared_ptr shares.\n";
  documents.create("notes/smart-pointers.md", existing);

  index::FtsSearch search(db);
  index::NavQueries nav(db);

  ScriptedChatClient chat;
  chat.replies.push_back(toolCall("search_documents", R"({"query":"smart pointers"})"));
  chat.replies.push_back(toolCall(
      "propose_draft",
      R"({"path":"notes/smart-pointers-intro.md","title":"Smart pointers","body":"See [[notes/smart-pointers.md]].","tags":["lang/cpp"],"type":"note"})"));

  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat);
  REQUIRE(agent.enabled());

  llm::AgentDocumentSnapshot snap;
  snap.isNew = true;
  const std::string id = agent.start("Create a document about smart pointers", snap);
  const auto view = waitDone(agent, id);
  REQUIRE(view.status == "done");
  REQUIRE(view.draft);
  REQUIRE(view.draft->path == "notes/smart-pointers-intro.md");
  REQUIRE(view.draft->title == "Smart pointers");
  REQUIRE(view.draft->body.find("smart-pointers.md") != std::string::npos);

  REQUIRE_THROWS_AS(documents.get("notes/smart-pointers-intro.md"),
                    vault::DocumentNotFoundError);
}

TEST_CASE("AgentRuntime: propose_draft unescapes over-escaped wiki-links",
          "[AgentRuntime]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService documents(repo, indexUpdater, snapshots);
  index::FtsSearch search(db);
  index::NavQueries nav(db);

  ScriptedChatClient chat;
  chat.replies.push_back(toolCall(
      "propose_draft",
      R"({"path":"notes/p.md","title":"Pointers","body":"see \\[\\[notes/programming/cpp/move\\-semantics\\.md\\|Move Semantics\\]\\]\\."})"));

  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat);
  llm::AgentDocumentSnapshot snap;
  snap.isNew = true;
  const std::string id = agent.start("draft", snap);
  const auto view = waitDone(agent, id);
  REQUIRE(view.status == "done");
  REQUIRE(view.draft);
  REQUIRE(view.draft->body ==
          "see [[notes/programming/cpp/move-semantics.md|Move Semantics]].");
}

TEST_CASE("AgentRuntime: propose_draft unescapes double-escaped wiki-links",
          "[AgentRuntime]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService documents(repo, indexUpdater, snapshots);
  index::FtsSearch search(db);
  index::NavQueries nav(db);

  ScriptedChatClient chat;
  chat.replies.push_back(toolCall(
      "propose_draft",
      R"({"path":"notes/p.md","title":"T","body":"> see \\\\[\\\\[notes/foo.md\\\\|Bar\\\\]\\\\]"})"));

  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat);
  llm::AgentDocumentSnapshot snap;
  snap.isNew = true;
  const std::string id = agent.start("draft", snap);
  const auto view = waitDone(agent, id);
  REQUIRE(view.status == "done");
  REQUIRE(view.draft);
  REQUIRE(view.draft->body == "> see [[notes/foo.md|Bar]]");
}

TEST_CASE("AgentRuntime: empty system prompt keeps the compiled default",
          "[AgentRuntime]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService documents(repo, indexUpdater, snapshots);
  index::FtsSearch search(db);
  index::NavQueries nav(db);

  ScriptedChatClient chat;
  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat, "  \n");
  llm::AgentDocumentSnapshot snap;
  const std::string id = agent.start("draft", snap);
  waitDone(agent, id);
  REQUIRE(chat.lastSystem.find("propose_draft") != std::string::npos);
  REQUIRE(chat.lastSystem.find("append_to_draft") != std::string::npos);
  REQUIRE(chat.lastSystem.find("mermaid") != std::string::npos);
}

TEST_CASE("AgentRuntime: non-empty system prompt replaces the compiled default",
          "[AgentRuntime]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService documents(repo, indexUpdater, snapshots);
  index::FtsSearch search(db);
  index::NavQueries nav(db);

  ScriptedChatClient chat;
  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat,
                          "CUSTOM PROMPT ONLY");
  llm::AgentDocumentSnapshot snap;
  const std::string id = agent.start("draft", snap);
  waitDone(agent, id);
  REQUIRE(chat.lastSystem == "CUSTOM PROMPT ONLY");
}

TEST_CASE("AgentRuntime: append_to_draft extends the snapshot body", "[AgentRuntime]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService documents(repo, indexUpdater, snapshots);
  index::FtsSearch search(db);
  index::NavQueries nav(db);

  ScriptedChatClient chat;
  chat.replies.push_back(toolCall("append_to_draft", R"({"text":"Second paragraph."})"));

  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat);
  llm::AgentDocumentSnapshot snap;
  snap.isNew = false;
  snap.path = "notes/x.md";
  snap.title = "X";
  snap.body = "First paragraph.";
  const std::string id = agent.start("add a paragraph at the end", snap);
  const auto view = waitDone(agent, id);
  REQUIRE(view.status == "done");
  REQUIRE(view.draft);
  REQUIRE(view.draft->body.find("First paragraph.") != std::string::npos);
  REQUIRE(view.draft->body.find("Second paragraph.") != std::string::npos);
  bool sawAppend = false;
  for (const auto& ev : view.events) {
    if (ev.type == "edit" && ev.data.value("op", "") == "append") sawAppend = true;
  }
  REQUIRE(sawAppend);
}

TEST_CASE("AgentRuntime: replace_in_draft uses the editor selection", "[AgentRuntime]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService documents(repo, indexUpdater, snapshots);
  index::FtsSearch search(db);
  index::NavQueries nav(db);

  ScriptedChatClient chat;
  chat.replies.push_back(toolCall("replace_in_draft", R"({"replacement":"fixed"})"));

  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat);
  llm::AgentDocumentSnapshot snap;
  snap.body = "alpha\nfix me\nomega";
  snap.selection = "fix me";
  const std::string id = agent.start("fix the examples", snap);
  const auto view = waitDone(agent, id);
  REQUIRE(view.status == "done");
  REQUIRE(view.draft);
  REQUIRE(view.draft->body == "alpha\nfixed\nomega");
}

TEST_CASE("AgentRuntime: replace_in_draft rejects an ambiguous find", "[AgentRuntime]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService documents(repo, indexUpdater, snapshots);
  index::FtsSearch search(db);
  index::NavQueries nav(db);

  ScriptedChatClient chat;
  chat.replies.push_back(
      toolCall("replace_in_draft", R"({"find":"same","replacement":"x"})"));

  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat);
  llm::AgentDocumentSnapshot snap;
  snap.body = "same same";
  const std::string id = agent.start("change one same", snap);
  const auto view = waitDone(agent, id);
  REQUIRE(view.status == "done");
  REQUIRE(view.draft);
  REQUIRE(view.draft->body == "same same");
}

TEST_CASE("AgentRuntime: cancel stops an in-flight complete", "[AgentRuntime]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService documents(repo, indexUpdater, snapshots);
  index::FtsSearch search(db);
  index::NavQueries nav(db);

  class BlockingChatClient : public llm::ChatClient {
   public:
    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> cancelled{false};

    llm::ChatCompletion complete(const std::vector<llm::ChatMessage>&,
                                 const nlohmann::json&) override {
      std::unique_lock lock(m);
      cv.wait(lock, [&] { return cancelled.load(); });
      throw std::runtime_error("cancelled");
    }
    void cancel() override {
      cancelled.store(true);
      cv.notify_all();
    }
  };

  BlockingChatClient chat;
  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat);
  llm::AgentDocumentSnapshot snap;
  const std::string id = agent.start("draft", snap);
  agent.cancel(id);
  const auto view = waitDone(agent, id);
  REQUIRE(view.status == "cancelled");
}
