#include "llm/AgentRuntime.h"

#include "index/FtsSearch.h"
#include "util/Uuid.h"
#include "vault/PathGuard.h"

#include <cctype>
#include <stdexcept>

namespace wikicore::llm {

namespace {

constexpr int kMaxSteps = 8;
constexpr int kSearchLimit = 8;
constexpr int kListLimit = 30;
constexpr int kMaxGetDocument = 6;

const char* kSystemPrompt =
    "You draft markdown for a personal wiki. Existing notes in the vault "
    "are the source of truth — search them before writing.\n"
    "When the draft is ready, call propose_draft with path, title, tags, "
    "type, and body. You cannot save files; propose_draft fills the editor "
    "immediately. The human will review and Save.\n"
    "propose_draft always replaces the whole editor. If the snapshot body "
    "is not empty, treat the instruction as an edit: keep the rest of the "
    "document and return the complete body, not a fragment.\n"
    "Link existing notes as literal wiki-links: [[vault/relative/path.md]] "
    "or [[path.md|Label]]. Never backslash-escape [, ], |, -, or . inside "
    "them — write [[notes/foo.md|Foo]], not \\[\\[notes/foo.md\\|Foo\\]\\].\n"
    "Blockquotes (callouts, warnings, quoted passages) are ordinary "
    "Markdown lines starting with '>'. Diagrams are a fenced code block "
    "whose info string is exactly mermaid — no extra words on that fence "
    "line, or it renders as a plain code block instead of a diagram.\n"
    "Reuse tags that already exist when they fit. Default type is \"note\". "
    "Write in the same language as the user's instruction. "
    "Do not claim you saved anything.";

// Models often over-escape markdown punctuation in JSON tool arguments
// (live: \[\[notes/foo.md\|Foo\]\] instead of [[notes/foo.md|Foo]]).
// Toast UI's markdown serializer then does the same again on a
// round-trip through WYSIWYG, so a single pass is not enough.
// WikiLinks only matches a literal "[[".
std::string unescapeMarkdownPunctuationOnce(const std::string& body) {
  std::string out;
  out.reserve(body.size());
  for (std::size_t i = 0; i < body.size(); ++i) {
    if (body[i] == '\\' && i + 1 < body.size()) {
      const char next = body[i + 1];
      if (next == '[' || next == ']' || next == '|' || next == '-' || next == '.' ||
          next == '(' || next == ')' || next == '`') {
        out.push_back(next);
        ++i;
        continue;
      }
    }
    out.push_back(body[i]);
  }
  return out;
}

std::string unescapeModelMarkdown(std::string body) {
  for (int pass = 0; pass < 8; ++pass) {
    std::string next = unescapeMarkdownPunctuationOnce(body);
    if (next == body) return next;
    body = std::move(next);
  }
  return body;
}

std::string snippetForModel(const index::SearchResultItem& item) {
  if (!item.snippetIsHighlighted) return item.snippet;
  std::string out;
  out.reserve(item.snippet.size());
  for (char c : item.snippet) {
    if (c == index::FtsSearch::kSnippetMatchStart ||
        c == index::FtsSearch::kSnippetMatchEnd) {
      out += "**";
    } else {
      out += c;
    }
  }
  return out;
}

nlohmann::json searchItemJson(const index::SearchResultItem& item) {
  return nlohmann::json{{"path", item.path},
                        {"title", item.title},
                        {"type", item.docType},
                        {"tags", item.tags},
                        {"snippet", snippetForModel(item)}};
}

std::string snapshotUserPrefix(const AgentDocumentSnapshot& snap) {
  nlohmann::json meta;
  meta["isNew"] = snap.isNew;
  meta["path"] = snap.path;
  meta["title"] = snap.title;
  meta["type"] = snap.type;
  meta["tags"] = snap.tags;
  std::string out = "Current editor snapshot (not yet saved unless this is an "
                    "existing document):\n";
  out += meta.dump(2);
  out += "\n\nCurrent body:\n";
  out += snap.body.empty() ? "(empty)" : snap.body;
  return out;
}

std::string trimCopy(std::string s) {
  std::size_t start = 0;
  while (start < s.size() &&
         std::isspace(static_cast<unsigned char>(s[start]))) {
    ++start;
  }
  std::size_t end = s.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    --end;
  }
  s.erase(end);
  s.erase(0, start);
  return s;
}

std::string resolveSystemPrompt(std::string prompt) {
  prompt = trimCopy(std::move(prompt));
  if (prompt.empty()) return kSystemPrompt;
  return prompt;
}

}  // namespace

struct AgentRuntime::Session {
  std::string id;
  std::string status = "running";
  std::vector<AgentEvent> events;
  std::optional<AgentDraft> draft;
  std::vector<ChatMessage> messages;
  AgentDocumentSnapshot snapshot;
  int getDocumentCount = 0;
  std::thread worker;
};

AgentRuntime::AgentRuntime(index::FtsSearch& search, vault::DocumentService& documents,
                           index::NavQueries& nav, index::IndexUpdater& indexUpdater,
                           index::McpAuditLog* auditLog, ChatClient* chat,
                           std::string systemPrompt)
    : search_(search),
      documents_(documents),
      nav_(nav),
      indexUpdater_(indexUpdater),
      auditLog_(auditLog),
      chat_(chat),
      systemPrompt_(resolveSystemPrompt(std::move(systemPrompt))) {}

AgentRuntime::~AgentRuntime() {
  std::vector<std::shared_ptr<Session>> toJoin;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [id, session] : sessions_) {
      (void)id;
      toJoin.push_back(session);
    }
    sessions_.clear();
  }
  for (auto& session : toJoin) {
    if (session->worker.joinable()) session->worker.join();
  }
}

nlohmann::json AgentRuntime::toolSchemas() {
  auto strProp = [](const std::string& desc) {
    return nlohmann::json{{"type", "string"}, {"description", desc}};
  };
  auto arrStr = [](const std::string& desc) {
    return nlohmann::json{
        {"type", "array"}, {"description", desc}, {"items", {{"type", "string"}}}};
  };

  nlohmann::json tools = nlohmann::json::array();
  tools.push_back({
      {"type", "function"},
      {"function",
       {{"name", "search_documents"},
        {"description",
         "Full-text search over the wiki. Returns matching paths, titles, "
         "tags, and a short snippet. Call this before writing."},
        {"parameters",
         {{"type", "object"},
          {"properties",
           {{"query", strProp("Search text")},
            {"tags", arrStr("Require all of these tags")},
            {"type", strProp("Filter by document type")}}},
          {"required", nlohmann::json::array({"query"})}}}}},
  });
  tools.push_back({
      {"type", "function"},
      {"function",
       {{"name", "get_document"},
        {"description",
         "Fetch one document's full markdown body and metadata by vault "
         "path or id."},
        {"parameters",
         {{"type", "object"},
          {"properties", {{"id_or_path", strProp("Document path or uuid")}}},
          {"required", nlohmann::json::array({"id_or_path"})}}}}},
  });
  tools.push_back({
      {"type", "function"},
      {"function",
       {{"name", "list_tags"},
        {"description", "Every tag in use, with document counts."},
        {"parameters", {{"type", "object"}, {"properties", nlohmann::json::object()}}}}},
  });
  tools.push_back({
      {"type", "function"},
      {"function",
       {{"name", "list_documents"},
        {"description", "Browse documents without a search query."},
        {"parameters",
         {{"type", "object"},
          {"properties",
           {{"tag", strProp("Filter by exact tag")},
            {"type", strProp("Filter by document type")},
            {"folder", strProp("Path prefix, e.g. notes/")}}}}}}},
  });
  tools.push_back({
      {"type", "function"},
      {"function",
       {{"name", "propose_draft"},
        {"description",
         "Fill the user's editor with this document. Does not save. Call "
         "this when the draft is ready. Wiki-links in body must be "
         "literal [[path.md]] or [[path.md|Label]], never backslash-escaped."},
        {"parameters",
         {{"type", "object"},
          {"properties",
           {{"path", strProp("Vault-relative path, e.g. notes/smart-pointers.md")},
            {"title", strProp("Document title")},
            {"body", strProp("Markdown body")},
            {"type", strProp("Document type, e.g. note")},
            {"tags", arrStr("Tags")}}},
          {"required", nlohmann::json::array({"path", "title", "body"})}}}}},
  });
  return tools;
}

std::string AgentRuntime::start(const std::string& instruction,
                                AgentDocumentSnapshot snapshot) {
  if (!chat_) throw std::runtime_error("agent not configured");
  if (instruction.empty()) throw std::runtime_error("instruction is required");

  auto session = std::make_shared<Session>();
  session->id = util::newUuidV4();
  session->snapshot = std::move(snapshot);
  session->messages.push_back(ChatMessage{"system", systemPrompt_, "", {}});
  const std::string userText = snapshotUserPrefix(session->snapshot) + "\n\nInstruction:\n" +
                               instruction;
  session->messages.push_back(ChatMessage{"user", userText, "", {}});
  session->events.push_back(AgentEvent{"user", nlohmann::json{{"text", instruction}}});

  {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& [id, existing] : sessions_) {
      (void)id;
      if (existing->status == "running") {
        throw std::runtime_error("an agent session is already running");
      }
    }
    sessions_[session->id] = session;
  }

  session->worker = std::thread([this, id = session->id]() { runLoop(id); });
  return session->id;
}

void AgentRuntime::send(const std::string& sessionId, const std::string& instruction,
                        AgentDocumentSnapshot snapshot) {
  if (!chat_) throw std::runtime_error("agent not configured");
  if (instruction.empty()) throw std::runtime_error("instruction is required");

  std::shared_ptr<Session> session;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) throw std::runtime_error("session not found");
    session = it->second;
    if (session->status == "running") {
      throw std::runtime_error("session is still running");
    }
  }

  if (session->worker.joinable()) session->worker.join();

  session->snapshot = std::move(snapshot);
  session->status = "running";
  session->getDocumentCount = 0;
  const std::string userText = snapshotUserPrefix(session->snapshot) + "\n\nInstruction:\n" +
                               instruction;
  session->messages.push_back(ChatMessage{"user", userText, "", {}});
  session->events.push_back(AgentEvent{"user", nlohmann::json{{"text", instruction}}});
  session->worker = std::thread([this, id = session->id]() { runLoop(id); });
}

std::optional<AgentSessionView> AgentRuntime::view(const std::string& sessionId) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sessions_.find(sessionId);
  if (it == sessions_.end()) return std::nullopt;
  AgentSessionView out;
  out.id = it->second->id;
  out.status = it->second->status;
  out.events = it->second->events;
  out.draft = it->second->draft;
  return out;
}

void AgentRuntime::drop(const std::string& sessionId) {
  std::shared_ptr<Session> session;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) return;
    session = it->second;
    sessions_.erase(it);
  }
  if (session && session->worker.joinable()) session->worker.join();
}

void AgentRuntime::runLoop(const std::string& sessionId) {
  std::shared_ptr<Session> session;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) return;
    session = it->second;
  }

  auto fail = [&](const std::string& message) {
    std::lock_guard<std::mutex> lock(mu_);
    session->status = "error";
    session->events.push_back(AgentEvent{"error", nlohmann::json{{"text", message}}});
  };

  try {
    const nlohmann::json tools = toolSchemas();
    bool finished = false;
    for (int step = 0; step < kMaxSteps && !finished; ++step) {
      std::vector<ChatMessage> messages;
      {
        std::lock_guard<std::mutex> lock(mu_);
        messages = session->messages;
      }
      const ChatCompletion completion = chat_->complete(messages, tools);

      if (!completion.toolCalls.empty()) {
        ChatMessage assistant;
        assistant.role = "assistant";
        assistant.content = completion.content;
        assistant.toolCalls = completion.toolCalls;
        {
          std::lock_guard<std::mutex> lock(mu_);
          session->messages.push_back(assistant);
        }
        for (const auto& call : completion.toolCalls) {
          nlohmann::json args = nlohmann::json::object();
          if (!call.arguments.empty()) {
            try {
              args = nlohmann::json::parse(call.arguments);
            } catch (const nlohmann::json::parse_error&) {
              args = nlohmann::json::object();
            }
          }
          std::string result;
          try {
            result = executeTool(*session, call.name, args);
          } catch (const std::exception& e) {
            result = std::string("error: ") + e.what();
          }
          ChatMessage toolMsg;
          toolMsg.role = "tool";
          toolMsg.toolCallId = call.id;
          toolMsg.content = result;
          std::lock_guard<std::mutex> lock(mu_);
          session->messages.push_back(toolMsg);
          if (call.name == "propose_draft" && session->draft) finished = true;
        }
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(mu_);
        session->messages.push_back(
            ChatMessage{"assistant", completion.content, "", {}});
        if (!completion.content.empty()) {
          session->events.push_back(
              AgentEvent{"assistant", nlohmann::json{{"text", completion.content}}});
        }
      }
      finished = true;
    }

    std::lock_guard<std::mutex> lock(mu_);
    if (session->status == "running") {
      session->status = "done";
      session->events.push_back(AgentEvent{"done", nlohmann::json::object()});
    }
  } catch (const std::exception& e) {
    fail(e.what());
  }
}

std::string AgentRuntime::executeTool(Session& session, const std::string& name,
                                      const nlohmann::json& args) {
  auto audit = [&](const std::string& tool, const std::string& path, bool ok,
                   const std::string& detail) {
    if (auditLog_) auditLog_->record("compose:" + tool, path, ok, detail);
  };

  if (name == "search_documents") {
    if (!args.contains("query") || !args["query"].is_string() ||
        args["query"].get<std::string>().empty()) {
      throw std::runtime_error("missing or empty query");
    }
    index::SearchQuery q;
    q.text = args["query"].get<std::string>();
    q.includePrivate = true;
    q.limit = kSearchLimit;
    if (args.contains("type") && args["type"].is_string()) {
      q.docType = args["type"].get<std::string>();
    }
    if (args.contains("tags") && args["tags"].is_array()) {
      q.tags = args["tags"].get<std::vector<std::string>>();
    }
    const auto results = search_.search(q);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& item : results) arr.push_back(searchItemJson(item));
    {
      std::lock_guard<std::mutex> lock(mu_);
      session.events.push_back(
          AgentEvent{"tool", nlohmann::json{{"name", name},
                                            {"detail", q.text},
                                            {"count", static_cast<int>(results.size())}}});
    }
    audit(name, "", true, q.text);
    return arr.dump(2);
  }

  if (name == "get_document") {
    if (session.getDocumentCount >= kMaxGetDocument) {
      throw std::runtime_error("get_document limit reached for this turn");
    }
    if (!args.contains("id_or_path") || !args["id_or_path"].is_string()) {
      throw std::runtime_error("missing id_or_path");
    }
    const std::string idOrPath = args["id_or_path"].get<std::string>();
    auto tryGet = [&](const std::string& path) -> std::optional<vault::DocumentRecord> {
      try {
        return documents_.get(path);
      } catch (const vault::DocumentNotFoundError&) {
        return std::nullopt;
      } catch (const vault::PathTraversalError&) {
        return std::nullopt;
      }
    };
    auto record = tryGet(idOrPath);
    if (!record) {
      if (const auto resolved = indexUpdater_.findPathByUuid(idOrPath)) {
        record = tryGet(*resolved);
      }
    }
    if (!record) {
      audit(name, idOrPath, false, "not found");
      throw std::runtime_error("document not found: " + idOrPath);
    }
    session.getDocumentCount++;
    std::string out;
    out += "Path: " + record->path + "\n";
    out += "Title: " + record->frontMatter.title + "\n";
    out += "Type: " + record->frontMatter.type + "\n";
    out += "Tags: ";
    for (size_t i = 0; i < record->frontMatter.tags.size(); ++i) {
      if (i) out += ", ";
      out += record->frontMatter.tags[i];
    }
    out += "\n\n---\n\n";
    out += record->body;
    {
      std::lock_guard<std::mutex> lock(mu_);
      session.events.push_back(
          AgentEvent{"tool", nlohmann::json{{"name", name}, {"detail", record->path}}});
    }
    audit(name, record->path, true, "");
    return out;
  }

  if (name == "list_tags") {
    const auto tags = nav_.tagCounts(true);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& t : tags) {
      arr.push_back(nlohmann::json{{"tag", t.tag}, {"count", t.count}});
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      session.events.push_back(AgentEvent{
          "tool", nlohmann::json{{"name", name}, {"count", static_cast<int>(tags.size())}}});
    }
    audit(name, "", true, "");
    return arr.dump(2);
  }

  if (name == "list_documents") {
    index::SearchQuery q;
    q.includePrivate = true;
    q.limit = kListLimit;
    if (args.contains("tag") && args["tag"].is_string()) {
      q.tag = args["tag"].get<std::string>();
    }
    if (args.contains("type") && args["type"].is_string()) {
      q.docType = args["type"].get<std::string>();
    }
    if (args.contains("folder") && args["folder"].is_string()) {
      q.folderPrefix = args["folder"].get<std::string>();
    }
    const auto results = search_.search(q);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& item : results) {
      arr.push_back(nlohmann::json{
          {"path", item.path}, {"title", item.title}, {"type", item.docType}, {"tags", item.tags}});
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      session.events.push_back(AgentEvent{
          "tool", nlohmann::json{{"name", name}, {"count", static_cast<int>(results.size())}}});
    }
    audit(name, q.folderPrefix.value_or(""), true, "");
    return arr.dump(2);
  }

  if (name == "propose_draft") {
    if (!args.contains("path") || !args["path"].is_string() ||
        !args.contains("title") || !args["title"].is_string() ||
        !args.contains("body") || !args["body"].is_string()) {
      throw std::runtime_error("propose_draft requires path, title, and body");
    }
    AgentDraft draft;
    draft.path = unescapeModelMarkdown(args["path"].get<std::string>());
    draft.title = unescapeModelMarkdown(args["title"].get<std::string>());
    draft.body = unescapeModelMarkdown(args["body"].get<std::string>());
    draft.type = args.value("type", std::string("note"));
    if (args.contains("tags") && args["tags"].is_array()) {
      draft.tags = args["tags"].get<std::vector<std::string>>();
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      session.draft = draft;
      session.events.push_back(AgentEvent{
          "draft", nlohmann::json{{"path", draft.path}, {"title", draft.title}}});
    }
    audit(name, draft.path, true, draft.title);
    return "Draft recorded for the editor. Stop. Do not claim the file was saved.";
  }

  throw std::runtime_error("unknown tool: " + name);
}

}  // namespace wikicore::llm
