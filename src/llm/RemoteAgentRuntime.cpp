#include "llm/RemoteAgentRuntime.h"

#include <nlohmann/json.hpp>

#include <filesystem>

namespace wikicore::llm {

namespace {

// Same bound as AgentRuntime::kMaxSteps -- a handful of tool-call
// round-trips is enough for "search, maybe ask one clarifying question,
// then answer or create," and a hard cap keeps a confused model from
// looping forever on someone else's bearer token.
constexpr int kMaxSteps = 8;
constexpr int kSearchLimit = 12;
constexpr int kListLimit = 30;

constexpr const char* kDefaultSystemPrompt =
    "You help with one personal wiki over a remote connection. You can "
    "search/browse/read documents, check the calendar, and create a new "
    "document. Use search_documents or list_documents before creating "
    "anything, to avoid making a near-duplicate of something that already "
    "exists -- if you find one, say so instead of creating a new one, "
    "unless the human clearly wants a separate document. When creating: "
    "infer a short, hyphenated, vault-relative path from the title if "
    "none was given (e.g. notes/my-title.md); ask ONE short clarifying "
    "question first only if something essential is genuinely ambiguous "
    "(most often: public or private); otherwise just create it. "
    "Markdown body, no raw HTML.";

// FtsSearch's snippet() return value uses raw control bytes
// (kSnippetMatchStart/End) as match markers instead of literal
// "<mark>"/"</mark>" -- see FtsSearch.h's own comment and CLAUDE.md's
// "FTS5 snippet()" note. Those bytes are meant for an HTML-escape-then-
// substitute render step (SearchRoutes.cpp), which doesn't exist here;
// swap them for plain markdown emphasis instead of handing a model raw
// non-printable bytes, same substitution AgentRuntime.cpp's own
// snippetForModel() already does for the identical reuse case.
std::string snippetForModel(const index::SearchResultItem& item) {
  if (!item.snippetIsHighlighted) return item.snippet;
  std::string out;
  out.reserve(item.snippet.size());
  for (char c : item.snippet) {
    if (c == index::FtsSearch::kSnippetMatchStart || c == index::FtsSearch::kSnippetMatchEnd) {
      out += "**";
    } else {
      out += c;
    }
  }
  return out;
}

nlohmann::json toolSchemas() {
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
         "Full-text search over the wiki's documents. Returns matching "
         "paths, titles, tags, and a short snippet -- not the full body. "
         "Call this before creating anything, to check for an existing "
         "near-duplicate."},
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
       {{"name", "list_tags"},
        {"description", "Every tag in use, with document counts."},
        {"parameters", {{"type", "object"}, {"properties", nlohmann::json::object()}}}}},
  });
  tools.push_back({
      {"type", "function"},
      {"function",
       {{"name", "get_document"},
        {"description", "Fetch one document's full markdown body and metadata by vault path or id."},
        {"parameters",
         {{"type", "object"},
          {"properties", {{"id_or_path", strProp("Document path or uuid")}}},
          {"required", nlohmann::json::array({"id_or_path"})}}}}},
  });
  tools.push_back({
      {"type", "function"},
      {"function",
       {{"name", "get_calendar_events"},
        {"description",
         "Calendar events (documents with a due date) in a date range, "
         "inclusive. Recurring series are already expanded into concrete "
         "per-day occurrences."},
        {"parameters",
         {{"type", "object"},
          {"properties",
           {{"start", strProp("Start date, inclusive, YYYY-MM-DD")},
            {"end", strProp("End date, inclusive, YYYY-MM-DD")},
            {"folder", strProp("Restrict to a path prefix, e.g. notes/")},
            {"tags", arrStr("Restrict to documents having all of these tags")}}},
          {"required", nlohmann::json::array({"start", "end"})}}}}},
  });
  tools.push_back({
      {"type", "function"},
      {"function",
       {{"name", "create_document"},
        {"description", "Create a new wiki document."},
        {"parameters",
         {{"type", "object"},
          {"properties",
           {{"path", strProp("Vault-relative path, e.g. notes/my-note.md")},
            {"title", strProp("Document title")},
            {"body", strProp("Markdown body")},
            {"type", strProp("Document type, e.g. note")},
            {"visibility", strProp("\"public\" or \"private\" (default private)")},
            {"tags", arrStr("Tags for this document")}}},
          {"required", nlohmann::json::array({"path", "title", "body"})}}}}},
  });
  return tools;
}

}  // namespace

RemoteAgentRuntime::RemoteAgentRuntime(ChatClient& chat, vault::DocumentService& documents,
                                       index::FtsSearch& search, index::NavQueries& nav,
                                       index::CalendarQueries& calendar,
                                       index::IndexUpdater& indexUpdater,
                                       index::McpAuditLog& auditLog, std::string systemPrompt)
    : chat_(chat),
      documents_(documents),
      search_(search),
      nav_(nav),
      calendar_(calendar),
      indexUpdater_(indexUpdater),
      auditLog_(auditLog),
      systemPrompt_(systemPrompt.empty() ? kDefaultSystemPrompt : std::move(systemPrompt)) {}

RemoteAgentTurn RemoteAgentRuntime::run(const std::vector<ChatMessage>& history,
                                        const std::string& tokenLabel) {
  std::vector<ChatMessage> messages;
  messages.push_back(ChatMessage{"system", systemPrompt_, "", {}});
  for (const auto& m : history) messages.push_back(m);

  const nlohmann::json tools = toolSchemas();
  RemoteAgentTurn turn;

  // Everything below can throw only from chat_.complete() itself (the
  // individual tool branches below catch their own exceptions and never
  // rethrow) -- a network error or a non-200 from the configured LLM
  // provider, whose message embeds that provider's own raw error body
  // (CloudChatClient.cpp). That text is useful for the admin, worthless
  // and a minor backend-detail leak to the remote caller, so it goes to
  // mcp_audit_log here and ONLY a generic message crosses back out to
  // RemoteAgentRoutes.cpp, same "internal detail never reaches the
  // caller" rule as filesystem_error elsewhere in this app.
  try {
    for (int step = 0; step < kMaxSteps; ++step) {
      const ChatCompletion completion = chat_.complete(messages, tools, {});

      if (completion.toolCalls.empty()) {
        turn.reply = completion.content;
        return turn;
      }

      ChatMessage assistant;
      assistant.role = "assistant";
      assistant.content = completion.content;
      assistant.toolCalls = completion.toolCalls;
      messages.push_back(assistant);

      for (const auto& call : completion.toolCalls) {
        nlohmann::json args = nlohmann::json::object();
        if (!call.arguments.empty()) {
          try {
            args = nlohmann::json::parse(call.arguments);
          } catch (const nlohmann::json::parse_error&) {
            args = nlohmann::json::object();
          }
        }

        if (call.name == "search_documents") {
          const std::string query = args.value("query", "");
          if (query.empty()) {
            messages.push_back(ChatMessage{"tool", "error: missing 'query'", call.id, {}});
            continue;
          }
          index::SearchQuery q;
          q.text = query;
          q.includePrivate = true;
          q.limit = kSearchLimit;
          if (args.contains("type") && args["type"].is_string()) {
            q.docType = args["type"].get<std::string>();
          }
          if (args.contains("tags") && args["tags"].is_array()) {
            for (const auto& t : args["tags"]) {
              if (t.is_string()) q.tags.push_back(t.get<std::string>());
            }
          }
          const auto results = search_.search(q);
          nlohmann::json arr = nlohmann::json::array();
          for (const auto& item : results) {
            arr.push_back(nlohmann::json{{"path", item.path},
                                          {"title", item.title},
                                          {"type", item.docType},
                                          {"tags", item.tags},
                                          {"snippet", snippetForModel(item)}});
          }
          auditLog_.record("remote-agent:search_documents", "", true, query, tokenLabel);
          messages.push_back(ChatMessage{"tool", arr.dump(2), call.id, {}});
          continue;
        }

        if (call.name == "list_documents") {
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
            arr.push_back(nlohmann::json{{"path", item.path},
                                          {"title", item.title},
                                          {"type", item.docType},
                                          {"tags", item.tags}});
          }
          auditLog_.record("remote-agent:list_documents", q.folderPrefix.value_or(""), true, "",
                            tokenLabel);
          messages.push_back(ChatMessage{"tool", arr.dump(2), call.id, {}});
          continue;
        }

        if (call.name == "list_tags") {
          const auto tagCounts = nav_.tagCounts(true);
          nlohmann::json arr = nlohmann::json::array();
          for (const auto& t : tagCounts) {
            arr.push_back(nlohmann::json{{"tag", t.tag}, {"count", t.count}});
          }
          auditLog_.record("remote-agent:list_tags", "", true, "", tokenLabel);
          messages.push_back(ChatMessage{"tool", arr.dump(2), call.id, {}});
          continue;
        }

        if (call.name == "get_document") {
          const std::string idOrPath = args.value("id_or_path", "");
          if (idOrPath.empty()) {
            messages.push_back(ChatMessage{"tool", "error: missing 'id_or_path'", call.id, {}});
            continue;
          }
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
            auditLog_.record("remote-agent:get_document", idOrPath, false, "not found",
                              tokenLabel);
            messages.push_back(
                ChatMessage{"tool", "error: document not found: " + idOrPath, call.id, {}});
            continue;
          }
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
          auditLog_.record("remote-agent:get_document", record->path, true, "", tokenLabel);
          messages.push_back(ChatMessage{"tool", out, call.id, {}});
          continue;
        }

        if (call.name == "get_calendar_events") {
          const std::string start = args.value("start", "");
          const std::string end = args.value("end", "");
          if (start.empty() || end.empty()) {
            messages.push_back(
                ChatMessage{"tool", "error: 'start' and 'end' are required", call.id, {}});
            continue;
          }
          const std::string folder = args.value("folder", "");
          std::vector<std::string> tags;
          if (args.contains("tags") && args["tags"].is_array()) {
            for (const auto& t : args["tags"]) {
              if (t.is_string()) tags.push_back(t.get<std::string>());
            }
          }
          const auto events = calendar_.eventsBetween(start, end, true, folder, tags);
          nlohmann::json arr = nlohmann::json::array();
          for (const auto& ev : events) {
            arr.push_back(nlohmann::json{{"path", ev.path},
                                          {"title", ev.title},
                                          {"visibility", ev.visibility},
                                          {"date", ev.date},
                                          {"time", ev.time}});
          }
          auditLog_.record("remote-agent:get_calendar_events", folder, true,
                            start + ".." + end, tokenLabel);
          messages.push_back(ChatMessage{"tool", arr.dump(2), call.id, {}});
          continue;
        }

        if (call.name != "create_document") {
          messages.push_back(ChatMessage{"tool", "error: unknown tool", call.id, {}});
          continue;
        }

        const std::string path = args.value("path", "");
        if (path.empty()) {
          messages.push_back(ChatMessage{"tool", "error: missing 'path'", call.id, {}});
          continue;
        }

        vault::DocumentInput input;
        input.title = args.value("title", "");
        input.body = args.value("body", "");
        input.type = args.value("type", "");
        input.visibility = args.value("visibility", "private");
        if (args.contains("tags") && args["tags"].is_array()) {
          for (const auto& t : args["tags"]) {
            if (t.is_string()) input.tags.push_back(t.get<std::string>());
          }
        }

        try {
          const auto rec = documents_.create(path, input);
          auditLog_.record("remote-agent:create_document", path, true, "created", tokenLabel);
          turn.reply = "Created " + rec.path;
          turn.documentsCreated.push_back(rec.path);
          return turn;
        } catch (const vault::DocumentAlreadyExistsError&) {
          auditLog_.record("remote-agent:create_document", path, false,
                            "a document already exists at that path", tokenLabel);
          messages.push_back(ChatMessage{
              "tool", "error: a document already exists at that path", call.id, {}});
        } catch (const vault::PathTraversalError&) {
          auditLog_.record("remote-agent:create_document", path, false,
                            "path traversal rejected", tokenLabel);
          messages.push_back(ChatMessage{"tool", "error: invalid path", call.id, {}});
        } catch (const std::filesystem::filesystem_error& e) {
          // Same "never leak the absolute vault path" rule as every other
          // write route -- see RemoteMcpRoutes.cpp's handleCreateDocument.
          auditLog_.record("remote-agent:create_document", path, false, e.what(), tokenLabel);
          messages.push_back(ChatMessage{"tool", "error: invalid path", call.id, {}});
        } catch (const std::exception& e) {
          auditLog_.record("remote-agent:create_document", path, false, e.what(), tokenLabel);
          messages.push_back(ChatMessage{"tool", std::string("error: ") + e.what(), call.id, {}});
        }
      }
    }
  } catch (const std::exception& e) {
    auditLog_.record("remote-agent:error", "", false, e.what(), tokenLabel);
    throw;
  }

  turn.reply =
      "I couldn't finish this within the step limit -- try again with fewer things to "
      "decide at once.";
  return turn;
}

}  // namespace wikicore::llm
