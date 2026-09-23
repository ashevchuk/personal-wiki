#include "index/Database.h"
#include "index/FtsSearch.h"
#include "index/IndexUpdater.h"
#include "index/NavQueries.h"
#include "index/SnapshotStore.h"
#include "index/AgentChatStore.h"
#include "index/McpAuditLog.h"
#include "index/QueryBlocks.h"
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
  std::string lastUser;
  std::string lastTool;
  nlohmann::json lastTools;

  llm::ChatCompletion complete(const std::vector<llm::ChatMessage>& messages,
                               const nlohmann::json& tools,
                               const llm::ChatDeltaFn& onDelta) override {
    lastTools = tools;
    for (const auto& msg : messages) {
      if (msg.role == "system") lastSystem = msg.content;
      if (msg.role == "user") lastUser = msg.content;
      if (msg.role == "tool") lastTool = msg.content;
    }
    if (index >= replies.size()) {
      llm::ChatCompletion done;
      done.content = "done";
      return done;
    }
    auto reply = replies[index++];
    if (onDelta && streamChunks && reply.toolCalls.empty() && !reply.content.empty()) {
      for (char c : reply.content) onDelta(std::string_view(&c, 1));
    }
    return reply;
  }
  bool streamChunks = false;
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
  REQUIRE_THROWS_AS(agent.startChat("hello"), std::runtime_error);
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
  REQUIRE(chat.lastSystem.find("insert_in_draft") != std::string::npos);
  REQUIRE(chat.lastSystem.find("until the human answers") != std::string::npos);
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

TEST_CASE("AgentRuntime: a text-only reply is visible and does not fill a draft",
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
  llm::ChatCompletion ask;
  ask.content = "Which folder should this note live in?";
  chat.replies.push_back(ask);

  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat);
  llm::AgentDocumentSnapshot snap;
  snap.isNew = true;
  const std::string id = agent.start("write something about pointers", snap);
  const auto view = waitDone(agent, id);
  REQUIRE(view.status == "done");
  REQUIRE_FALSE(view.draft);
  bool sawAsk = false;
  for (const auto& ev : view.events) {
    if (ev.type == "assistant" &&
        ev.data.value("text", "").find("Which folder") != std::string::npos) {
      sawAsk = true;
    }
  }
  REQUIRE(sawAsk);
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
                                 const nlohmann::json&,
                                 const llm::ChatDeltaFn&) override {
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

TEST_CASE("AgentRuntime: insert_in_draft splices at the caret prefix", "[AgentRuntime]") {
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
  chat.replies.push_back(toolCall("insert_in_draft", R"({"text":"INSERTED\n"})"));

  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat);
  llm::AgentDocumentSnapshot snap;
  snap.body = "alpha\nomega";
  snap.caretBefore = std::string("alpha\n");
  const std::string id = agent.start("insert a paragraph here", snap);
  const auto view = waitDone(agent, id);
  REQUIRE(view.status == "done");
  REQUIRE(view.draft);
  REQUIRE(view.draft->body == "alpha\nINSERTED\nomega");
}

TEST_CASE("AgentRuntime: insert_in_draft at the start", "[AgentRuntime]") {
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
  chat.replies.push_back(toolCall("insert_in_draft", R"({"text":"HEAD\n"})"));

  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat);
  llm::AgentDocumentSnapshot snap;
  snap.body = "body";
  snap.caretBefore = std::string("");
  const std::string id = agent.start("insert at the top", snap);
  const auto view = waitDone(agent, id);
  REQUIRE(view.status == "done");
  REQUIRE(view.draft);
  REQUIRE(view.draft->body == "HEAD\nbody");
}

TEST_CASE("AgentRuntime: insert_in_draft without a caret does not change the body",
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
  chat.replies.push_back(toolCall("insert_in_draft", R"({"text":"NOPE"})"));

  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat);
  llm::AgentDocumentSnapshot snap;
  snap.body = "keep me";
  const std::string id = agent.start("insert here", snap);
  const auto view = waitDone(agent, id);
  REQUIRE(view.status == "done");
  REQUIRE_FALSE(view.draft);
}

TEST_CASE("AgentRuntime: a selection sends an excerpt, not the whole body",
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
  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat);
  llm::AgentDocumentSnapshot snap;
  snap.body = std::string(5000, 'a') + "UNIQUE_SEL" + std::string(5000, 'b');
  snap.selection = "UNIQUE_SEL";
  const std::string id = agent.start("fix this", snap);
  waitDone(agent, id);
  REQUIRE(chat.lastUser.find("UNIQUE_SEL") != std::string::npos);
  REQUIRE(chat.lastUser.find("[...]") != std::string::npos);
  REQUIRE(chat.lastUser.find(std::string(2000, 'a')) == std::string::npos);
  REQUIRE(chat.lastUser.find(std::string(2000, 'b')) == std::string::npos);
}

TEST_CASE("AgentRuntime: list_types returns types in use with counts", "[AgentRuntime]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService documents(repo, indexUpdater, snapshots);

  vault::DocumentInput note;
  note.title = "A";
  note.visibility = "private";
  note.type = "note";
  note.body = "one\n";
  documents.create("notes/a.md", note);
  vault::DocumentInput howto;
  howto.title = "B";
  howto.visibility = "private";
  howto.type = "howto";
  howto.body = "two\n";
  documents.create("notes/b.md", howto);
  vault::DocumentInput note2;
  note2.title = "C";
  note2.visibility = "private";
  note2.type = "note";
  note2.body = "three\n";
  documents.create("notes/c.md", note2);

  index::FtsSearch search(db);
  index::NavQueries nav(db);
  ScriptedChatClient chat;
  chat.replies.push_back(toolCall("list_types", "{}"));

  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat);
  llm::AgentDocumentSnapshot snap;
  const std::string id = agent.start("what types exist", snap);
  const auto view = waitDone(agent, id);
  REQUIRE(view.status == "done");
  REQUIRE(chat.lastTool.find("\"type\": \"note\"") != std::string::npos);
  REQUIRE(chat.lastTool.find("\"count\": 2") != std::string::npos);
  REQUIRE(chat.lastTool.find("\"type\": \"howto\"") != std::string::npos);
}

TEST_CASE("AgentRuntime: content deltas coalesce into one assistant event",
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
  llm::ChatCompletion reply;
  reply.content = "Hello";
  chat.replies.push_back(reply);
  chat.streamChunks = true;

  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat);
  llm::AgentDocumentSnapshot snap;
  const std::string id = agent.start("say hello", snap);
  const auto view = waitDone(agent, id);
  REQUIRE(view.status == "done");
  int assistantCount = 0;
  std::string lastText;
  bool stillStreaming = false;
  for (const auto& ev : view.events) {
    if (ev.type != "assistant") continue;
    assistantCount++;
    lastText = ev.data.value("text", "");
    stillStreaming = ev.data.value("streaming", false);
  }
  REQUIRE(assistantCount == 1);
  REQUIRE(lastText == "Hello");
  REQUIRE_FALSE(stillStreaming);
}

TEST_CASE("AgentRuntime: waitGeneration wakes when a delta arrives", "[AgentRuntime]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService documents(repo, indexUpdater, snapshots);
  index::FtsSearch search(db);
  index::NavQueries nav(db);

  class SlowStreamClient : public llm::ChatClient {
   public:
    llm::ChatCompletion complete(const std::vector<llm::ChatMessage>&,
                                 const nlohmann::json&,
                                 const llm::ChatDeltaFn& onDelta) override {
      if (onDelta) onDelta("Hel");
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      if (onDelta) onDelta("lo");
      llm::ChatCompletion out;
      out.content = "Hello";
      return out;
    }
  };

  SlowStreamClient chat;
  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat);
  llm::AgentDocumentSnapshot snap;
  const std::string id = agent.start("say hello", snap);
  const auto gen = agent.generation(id);
  REQUIRE(agent.waitGeneration(id, gen, std::chrono::seconds(2)));
  auto mid = agent.view(id);
  REQUIRE(mid);
  bool sawPartial = false;
  for (const auto& ev : mid->events) {
    if (ev.type == "assistant" && ev.data.value("text", std::string()).find("Hel") == 0) {
      sawPartial = true;
    }
  }
  REQUIRE(sawPartial);
  const auto view = waitDone(agent, id);
  REQUIRE(view.status == "done");
}

TEST_CASE("AgentRuntime: startChat uses the chat system prompt and omits write tools",
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
  llm::ChatCompletion reply;
  reply.content = "See [[notes/a.md]].";
  chat.replies.push_back(reply);

  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat, "",
                          "  \n");
  const std::string id = agent.startChat("what is in the vault?");
  const auto view = waitDone(agent, id);
  REQUIRE(view.status == "done");
  REQUIRE(view.kind == "chat");
  REQUIRE_FALSE(view.draft);
  REQUIRE(chat.lastSystem.find("You cannot create, edit, or save files") !=
          std::string::npos);
  REQUIRE(chat.lastSystem.find("get_current_view") != std::string::npos);
  REQUIRE(chat.lastSystem.find("run_query_block") != std::string::npos);
  REQUIRE(chat.lastSystem.find("diff_document_history") != std::string::npos);
  REQUIRE(chat.lastSystem.find("propose_draft") == std::string::npos);
  REQUIRE(chat.lastUser.find("what is in the vault?") != std::string::npos);
  REQUIRE(chat.lastUser.find("Currently open") != std::string::npos);
  const std::string dumped = chat.lastTools.dump();
  REQUIRE(dumped.find("search_documents") != std::string::npos);
  REQUIRE(dumped.find("get_document") != std::string::npos);
  REQUIRE(dumped.find("run_query_block") != std::string::npos);
  REQUIRE(dumped.find("list_document_history") != std::string::npos);
  REQUIRE(dumped.find("diff_document_history") != std::string::npos);
  REQUIRE(dumped.find("get_current_view") != std::string::npos);
  REQUIRE(dumped.find("propose_draft") == std::string::npos);
  REQUIRE(dumped.find("append_to_draft") == std::string::npos);
  REQUIRE(dumped.find("insert_in_draft") == std::string::npos);
  REQUIRE(dumped.find("replace_in_draft") == std::string::npos);
}

TEST_CASE("AgentRuntime: non-empty chat_system_prompt replaces the compiled default",
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
                          "DRAFT ONLY", "CHAT ONLY");
  const std::string id = agent.startChat("hello");
  waitDone(agent, id);
  REQUIRE(chat.lastSystem == "CHAT ONLY");
}

TEST_CASE("AgentRuntime: chat rejects write tools even if the model calls them",
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
      R"({"path":"notes/x.md","title":"X","body":"nope"})"));

  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat);
  const std::string id = agent.startChat("write a note");
  const auto view = waitDone(agent, id);
  REQUIRE(view.status == "done");
  REQUIRE_FALSE(view.draft);
  REQUIRE(chat.lastTool.find("write tools are not available in chat") != std::string::npos);
}

TEST_CASE("AgentRuntime: chat tool calls are audited with a chat: prefix",
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
  index::McpAuditLog audit(db);

  ScriptedChatClient chat;
  chat.replies.push_back(toolCall("list_tags", "{}"));

  llm::AgentRuntime agent(search, documents, nav, indexUpdater, &audit, &chat);
  const std::string id = agent.startChat("list tags");
  waitDone(agent, id);
  const auto rows = audit.listRecent(10);
  REQUIRE_FALSE(rows.empty());
  REQUIRE(rows.front().toolName == "chat:list_tags");
  REQUIRE(rows.front().success);
}

TEST_CASE("AgentRuntime: send on a chat session is rejected", "[AgentRuntime]") {
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
  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat);
  const std::string id = agent.startChat("hello");
  waitDone(agent, id);
  llm::AgentDocumentSnapshot snap;
  try {
    agent.send(id, "follow up", snap);
    FAIL("send on a chat session should throw");
  } catch (const std::runtime_error& e) {
    REQUIRE(std::string(e.what()) == "not a draft session");
  }
  try {
    agent.sendChat("missing", "follow up");
    FAIL("sendChat on a missing session should throw");
  } catch (const std::runtime_error& e) {
    REQUIRE(std::string(e.what()) == "session not found");
  }
}

TEST_CASE("AgentRuntime: chat get_current_view returns the open wiki page",
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
  chat.replies.push_back(toolCall("get_current_view", "{}"));

  llm::AgentUiContext ui;
  ui.page = "document";
  ui.path = "demo/wiki-links-example/note-a.md";
  ui.title = "Wiki Links Example - A";
  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat);
  const std::string id = agent.startChat("what is in this document?", ui);
  const auto view = waitDone(agent, id);
  REQUIRE(view.status == "done");
  REQUIRE(chat.lastUser.find("demo/wiki-links-example/note-a.md") != std::string::npos);
  REQUIRE(chat.lastUser.find("what is in this document?") != std::string::npos);
  REQUIRE(chat.lastTool.find("\"page\": \"document\"") != std::string::npos);
  REQUIRE(chat.lastTool.find("note-a.md") != std::string::npos);
}

TEST_CASE("AgentRuntime: sendChat refreshes the open wiki page", "[AgentRuntime]") {
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
  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat);
  llm::AgentUiContext first;
  first.page = "document";
  first.path = "notes/a.md";
  const std::string id = agent.startChat("hello", first);
  waitDone(agent, id);

  chat.replies.push_back(toolCall("get_current_view", "{}"));
  llm::AgentUiContext second;
  second.page = "folder";
  second.path = "notes/";
  agent.sendChat(id, "what is in this folder?", second);
  waitDone(agent, id);
  REQUIRE(chat.lastUser.find("\"page\": \"folder\"") != std::string::npos);
  REQUIRE(chat.lastUser.find("notes/") != std::string::npos);
  REQUIRE(chat.lastTool.find("\"page\": \"folder\"") != std::string::npos);
}

TEST_CASE("AgentRuntime: draft tool list does not include get_current_view",
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
  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat);
  llm::AgentDocumentSnapshot snap;
  const std::string id = agent.start("draft", snap);
  waitDone(agent, id);
  const std::string dumped = chat.lastTools.dump();
  REQUIRE(dumped.find("propose_draft") != std::string::npos);
  REQUIRE(dumped.find("run_query_block") != std::string::npos);
  REQUIRE(dumped.find("diff_document_history") != std::string::npos);
  REQUIRE(dumped.find("get_current_view") == std::string::npos);
}

TEST_CASE("AgentRuntime: chat sessions persist, hydrate, rename, and drop",
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
  index::AgentChatStore store(db);

  std::string firstId;
  std::string secondId;
  {
    ScriptedChatClient chat;
    llm::ChatCompletion reply;
    reply.content = "first answer";
    chat.replies.push_back(reply);
    llm::ChatCompletion reply2;
    reply2.content = "second answer";
    chat.replies.push_back(reply2);
    llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat, "", "",
                            &store);
    firstId = agent.startChat("what is in the vault?");
    auto view = waitDone(agent, firstId);
    REQUIRE(view.title == "what is in the vault?");
    REQUIRE(view.kind == "chat");
    secondId = agent.startChat("list tags please");
    waitDone(agent, secondId);
    const auto listed = agent.listChats();
    REQUIRE(listed.size() == 2);
    REQUIRE(listed[0].id == secondId);
    REQUIRE(listed[1].id == firstId);
  }

  ScriptedChatClient chat;
  llm::ChatCompletion follow;
  follow.content = "hydrated";
  chat.replies.push_back(follow);
  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat, "",
                          "hydrated-system-prompt", &store);
  REQUIRE(agent.loadChat(firstId));
  auto restored = agent.view(firstId);
  REQUIRE(restored);
  REQUIRE(restored->status == "done");
  REQUIRE(restored->title == "what is in the vault?");
  bool sawUser = false;
  bool sawAssistant = false;
  for (const auto& ev : restored->events) {
    if (ev.type == "user") sawUser = true;
    if (ev.type == "assistant" &&
        ev.data.value("text", std::string()) == "first answer") {
      sawAssistant = true;
    }
  }
  REQUIRE(sawUser);
  REQUIRE(sawAssistant);

  agent.renameChat(firstId, "Vault overview");
  REQUIRE(agent.listChats().size() == 2);
  bool renamed = false;
  for (const auto& row : agent.listChats()) {
    if (row.id == firstId) {
      REQUIRE(row.title == "Vault overview");
      renamed = true;
    }
  }
  REQUIRE(renamed);

  agent.sendChat(firstId, "follow up");
  waitDone(agent, firstId);
  REQUIRE(chat.lastSystem == "hydrated-system-prompt");

  agent.drop(firstId);
  REQUIRE_FALSE(agent.loadChat(firstId));
  REQUIRE_FALSE(agent.view(firstId));
  const auto remaining = agent.listChats();
  REQUIRE(remaining.size() == 1);
  REQUIRE(remaining[0].id == secondId);
}

TEST_CASE("AgentRuntime: drop of a draft does not wipe persisted chats",
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
  index::AgentChatStore store(db);

  ScriptedChatClient chat;
  llm::ChatCompletion reply;
  reply.content = "ok";
  chat.replies.push_back(reply);
  chat.replies.push_back(reply);
  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat, "", "",
                          &store);
  const std::string chatId = agent.startChat("hello");
  waitDone(agent, chatId);
  llm::AgentDocumentSnapshot snap;
  const std::string draftId = agent.start("draft this", snap);
  waitDone(agent, draftId);
  agent.drop(draftId);
  REQUIRE(agent.loadChat(chatId));
  REQUIRE(agent.listChats().size() == 1);
}

TEST_CASE("AgentRuntime: run_query_block executes the query DSL, not the fence",
          "[AgentRuntime]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService documents(repo, indexUpdater, snapshots);

  vault::DocumentInput recipe;
  recipe.title = "Borscht";
  recipe.visibility = "private";
  recipe.type = "recipe";
  recipe.body = "Beets.\n";
  documents.create("recipes/borscht.md", recipe);
  vault::DocumentInput note;
  note.title = "Other";
  note.visibility = "private";
  note.type = "note";
  note.body = "Not a recipe.\n";
  documents.create("notes/other.md", note);

  index::FtsSearch search(db);
  index::NavQueries nav(db);
  index::QueryBlocks qb(db, search);

  ScriptedChatClient chat;
  chat.replies.push_back(toolCall(
      "run_query_block", "{\"query\":\"```query\\ntype: recipe\\n```\"}"));
  llm::ChatCompletion done;
  done.content = "ok";
  chat.replies.push_back(done);

  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat, "", "",
                          nullptr, &qb, &snapshots);
  const std::string id = agent.startChat("what recipes exist?");
  const auto view = waitDone(agent, id);
  REQUIRE(view.status == "done");
  REQUIRE(chat.lastTool.find("recipes/borscht.md") != std::string::npos);
  REQUIRE(chat.lastTool.find("notes/other.md") == std::string::npos);
}

TEST_CASE("AgentRuntime: run_query_block surfaces a DSL typo, not an empty list",
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
  index::QueryBlocks qb(db, search);

  ScriptedChatClient chat;
  chat.replies.push_back(toolCall("run_query_block", R"({"query":"tags: cpp"})"));
  llm::ChatCompletion done;
  done.content = "ok";
  chat.replies.push_back(done);

  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat, "", "",
                          nullptr, &qb, &snapshots);
  const std::string id = agent.startChat("run this query");
  waitDone(agent, id);
  REQUIRE(chat.lastTool.find("error:") != std::string::npos);
  REQUIRE(chat.lastTool.find("[]") == std::string::npos);
}

TEST_CASE("AgentRuntime: list and diff document history against current",
          "[AgentRuntime]") {
  TempEnv env;
  index::Database db(env.dbPath());
  db.migrate();
  index::IndexUpdater indexUpdater(db);
  index::SnapshotStore snapshots(db);
  vault::VaultRepository repo(env.vaultRoot());
  vault::DocumentService documents(repo, indexUpdater, snapshots);

  vault::DocumentInput first;
  first.title = "Note";
  first.visibility = "private";
  first.type = "note";
  first.body = "alpha\nshared\n";
  documents.create("notes/hist.md", first);
  vault::DocumentInput second;
  second.title = "Note";
  second.visibility = "private";
  second.type = "note";
  second.body = "beta\nshared\n";
  documents.update("notes/hist.md", second);

  index::FtsSearch search(db);
  index::NavQueries nav(db);
  index::QueryBlocks qb(db, search);

  ScriptedChatClient chat;
  chat.replies.push_back(
      toolCall("list_document_history", R"({"path":"notes/hist.md"})"));
  llm::ChatCompletion listed;
  listed.content = "listed";
  chat.replies.push_back(listed);

  llm::AgentRuntime agent(search, documents, nav, indexUpdater, nullptr, &chat, "", "",
                          nullptr, &qb, &snapshots);
  const std::string id = agent.startChat("history of this note");
  auto view = waitDone(agent, id);
  REQUIRE(view.status == "done");
  REQUIRE(chat.lastTool.find("\"id\"") != std::string::npos);
  REQUIRE(chat.lastTool.find("snapshotAt") != std::string::npos);

  chat.replies.push_back(
      toolCall("diff_document_history", R"({"path":"notes/hist.md"})"));
  llm::ChatCompletion diffed;
  diffed.content = "diffed";
  chat.replies.push_back(diffed);
  agent.sendChat(id, "show the diff");
  view = waitDone(agent, id);
  REQUIRE(view.status == "done");
  REQUIRE(chat.lastTool.find("- alpha") != std::string::npos);
  REQUIRE(chat.lastTool.find("+ beta") != std::string::npos);
  REQUIRE(chat.lastTool.find("  shared") != std::string::npos);
}

