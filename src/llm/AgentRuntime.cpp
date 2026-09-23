#include "llm/AgentRuntime.h"

#include "index/FtsSearch.h"
#include "util/Time.h"
#include "util/Uuid.h"
#include "vault/PathGuard.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace wikicore::llm {

namespace {

constexpr int kMaxSteps = 8;
constexpr int kSearchLimit = 12;
constexpr int kListLimit = 30;
constexpr int kMaxGetDocument = 6;
constexpr int kMaxQueryBlocks = 4;
constexpr int kMaxHistoryDiffs = 4;
constexpr std::size_t kMaxDiffChars = 20000;
constexpr std::size_t kMaxDiffCells = 800000;

const char* kSystemPrompt =
    "You draft markdown for a personal wiki. Existing notes in the vault "
    "are the source of truth — search them before writing.\n"
    "You cannot save files. The human reviews the editor and hits Save.\n"
    "For a new document or a full rewrite, call propose_draft with path, "
    "title, tags, type, and the complete body.\n"
    "For a follow-up that only adds or changes part of the current body, "
    "do not call propose_draft. Use append_to_draft to add at the end, "
    "insert_in_draft to insert at the caret, or replace_in_draft to change "
    "one unique span. replace_in_draft's find must appear exactly once in "
    "the current body; if the snapshot has selected text, omit find and "
    "that selection is the span. insert_in_draft uses the snapshot caret "
    "when there is no selection.\n"
    "Link existing notes as literal wiki-links: [[vault/relative/path.md]] "
    "or [[path.md|Label]]. Never backslash-escape [, ], |, -, or . inside "
    "them — write [[notes/foo.md|Foo]], not \\[\\[notes/foo.md\\|Foo\\]\\].\n"
    "Blockquotes (callouts, warnings, quoted passages) are ordinary "
    "Markdown lines starting with '>'. Diagrams are a fenced code block "
    "whose info string is exactly mermaid — no extra words on that fence "
    "line, or it renders as a plain code block instead of a diagram.\n"
    "Reuse tags that already exist when they fit. Default type is \"note\". "
    "Write in the same language as the user's instruction. "
    "If the instruction is ambiguous — which document or span, what to "
    "keep versus replace, missing path/title, or two reasonable readings "
    "— ask one to three short questions in the user's language and stop. "
    "Do not call propose_draft, append_to_draft, insert_in_draft, or "
    "replace_in_draft until the human answers in this panel. Search and "
    "get_document are fine first, so the questions can be specific. A "
    "```query block on a page is a live table: run_query_block with its "
    "body, not get_document alone. list_document_history and "
    "diff_document_history show past versions of one note. Do "
    "not stall on a clear request: a non-empty selection plus \"fix "
    "this\", \"add a paragraph at the end\", \"insert a paragraph here\" "
    "with a known caret, or an explicit full rewrite is enough to act. "
    "Do not claim you saved anything.";

const char* kChatSystemPrompt =
    "You answer questions about a personal wiki. Existing notes are the "
    "source of truth — search them before answering.\n"
    "You cannot create, edit, or save files. If the human wants a new "
    "note or a change, tell them to open the editor; do not claim you "
    "wrote anything.\n"
    "Each user turn includes a Currently open block for the wiki page "
    "behind this chat panel. get_current_view returns the same facts. "
    "If they say this document, this page, this folder, here, or the "
    "current note, that IS the open view — call get_document on a "
    "document/edit/history path, or list_documents with folder on a "
    "folder path. Do not ask which document or folder when a path is "
    "already available.\n"
    "A ```query fenced block is a live table, not the answer. "
    "get_document only shows the key: value DSL. Call run_query_block "
    "with that block's body to get the matching documents. "
    "list_document_history lists past snapshots; diff_document_history "
    "shows a snapshot versus the current body. Do not restore or edit "
    "from this panel.\n"
    "Cite notes as literal wiki-links: [[vault/relative/path.md]] or "
    "[[path.md|Label]]. Never backslash-escape [, ], |, -, or . inside "
    "them — write [[notes/foo.md|Foo]], not \\[\\[notes/foo.md\\|Foo\\]\\].\n"
    "If the question is still ambiguous after that view (two readings, "
    "missing facts not on the open page), ask one to three short "
    "questions in the user's language. Write in the same language as "
    "the question. Do not invent documents, paths, or facts that are "
    "not in tool results.";

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

constexpr std::size_t kBodyExcerptPad = 800;
constexpr std::size_t kCaretPrefixPad = 400;

std::string excerptAroundSelection(const std::string& body, const std::string& selection) {
  if (selection.empty() || body.empty()) return body;
  const auto pos = body.find(selection);
  if (pos == std::string::npos) return body;
  std::size_t start = pos > kBodyExcerptPad ? pos - kBodyExcerptPad : 0;
  std::size_t end = std::min(body.size(), pos + selection.size() + kBodyExcerptPad);
  if (start > 0) {
    const auto nl = body.find('\n', start);
    if (nl != std::string::npos && nl < pos) start = nl + 1;
  }
  if (end < body.size()) {
    const auto nl = body.rfind('\n', end);
    if (nl != std::string::npos && nl > pos + selection.size()) end = nl;
  }
  std::string out;
  if (start > 0) out += "[...]\n";
  out.append(body, start, end - start);
  if (end < body.size()) out += "\n[...]";
  return out;
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
  const bool excerpted = !snap.selection.empty() &&
                         snap.body.find(snap.selection) != std::string::npos;
  out += excerpted ? "\n\nCurrent body (excerpt around the selection; the rest "
                     "of the document is unchanged):\n"
                   : "\n\nCurrent body:\n";
  if (snap.body.empty()) {
    out += "(empty)";
  } else if (excerpted) {
    out += excerptAroundSelection(snap.body, snap.selection);
  } else {
    out += snap.body;
  }
  if (!snap.selection.empty()) {
    out += "\n\nSelected text in the editor (the instruction is about this "
           "span unless the user says otherwise):\n";
    out += snap.selection;
  } else if (snap.caretBefore) {
    out += "\n\nThe editor caret is at this insert point (use insert_in_draft). "
           "Text immediately before the caret:\n";
    const std::string& before = *snap.caretBefore;
    if (before.empty()) {
      out += "(start of document)";
    } else if (before.size() <= kCaretPrefixPad) {
      out += before;
    } else {
      out += "[...]\n";
      out.append(before, before.size() - kCaretPrefixPad, kCaretPrefixPad);
    }
  }
  return out;
}

std::size_t countOccurrences(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return 0;
  std::size_t count = 0;
  std::size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

void appendBlock(std::string& body, const std::string& extra) {
  if (!body.empty() && body.back() != '\n') body.push_back('\n');
  if (!body.empty()) body.push_back('\n');
  body += extra;
}

std::size_t insertOffset(const std::string& body, const std::string& before) {
  if (before.empty()) return 0;
  if (body.size() >= before.size() && body.compare(0, before.size(), before) == 0) {
    return before.size();
  }
  const std::size_t hits = countOccurrences(body, before);
  if (hits == 0) {
    throw std::runtime_error("caret prefix not found in the current body");
  }
  if (hits > 1) {
    throw std::runtime_error("caret prefix matches " + std::to_string(hits) +
                             " times — select a span or insert at the end");
  }
  return body.find(before) + before.size();
}

void ensureDraftFromSnapshot(std::optional<AgentDraft>& draft,
                             const AgentDocumentSnapshot& snapshot) {
  if (draft) return;
  AgentDraft next;
  next.path = snapshot.path;
  next.title = snapshot.title;
  next.type = snapshot.type.empty() ? "note" : snapshot.type;
  next.tags = snapshot.tags;
  next.body = snapshot.body;
  draft = std::move(next);
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

std::string resolveChatSystemPrompt(std::string prompt) {
  prompt = trimCopy(std::move(prompt));
  if (prompt.empty()) return kChatSystemPrompt;
  return prompt;
}

nlohmann::json uiContextJson(const AgentUiContext& ui) {
  nlohmann::json meta;
  meta["page"] = ui.page.empty() ? "other" : ui.page;
  if (!ui.path.empty()) meta["path"] = ui.path;
  if (!ui.title.empty()) meta["title"] = ui.title;
  return meta;
}

std::string chatUserText(const AgentUiContext& ui, const std::string& instruction) {
  std::string out =
      "Currently open in the wiki UI (not the chat panel):\n";
  out += uiContextJson(ui).dump(2);
  out += "\n\nQuestion:\n";
  out += instruction;
  return out;
}

std::string titleFromInstruction(std::string s) {
  s = trimCopy(std::move(s));
  std::string flat;
  bool space = false;
  for (unsigned char c : s) {
    if (std::isspace(c)) {
      if (!flat.empty()) space = true;
      continue;
    }
    if (space) {
      flat.push_back(' ');
      space = false;
    }
    flat.push_back(static_cast<char>(c));
  }
  constexpr std::size_t kMax = 72;
  if (flat.size() <= kMax) return flat;
  while (flat.size() > kMax - 1 &&
         (static_cast<unsigned char>(flat.back()) & 0xC0) == 0x80) {
    flat.pop_back();
  }
  if (flat.size() > kMax - 1) flat.resize(kMax - 1);
  while (!flat.empty() && (static_cast<unsigned char>(flat.back()) & 0xC0) == 0x80) {
    flat.pop_back();
  }
  flat += "…";
  return flat;
}

std::string stripQueryFence(std::string s) {
  s = trimCopy(std::move(s));
  if (s.size() >= 3 && s.compare(0, 3, "```") == 0) {
    const auto nl = s.find('\n');
    if (nl == std::string::npos) return {};
    s = s.substr(nl + 1);
    s = trimCopy(std::move(s));
    if (s.size() >= 3 && s.compare(s.size() - 3, 3, "```") == 0) {
      s.resize(s.size() - 3);
      s = trimCopy(std::move(s));
    }
  }
  return s;
}

std::vector<std::string> splitLines(const std::string& s) {
  std::vector<std::string> lines;
  std::string cur;
  for (char c : s) {
    if (c == '\n') {
      lines.push_back(cur);
      cur.clear();
    } else if (c != '\r') {
      cur.push_back(c);
    }
  }
  lines.push_back(std::move(cur));
  return lines;
}

std::string lineDiff(const std::string& oldText, const std::string& newText) {
  const auto a = splitLines(oldText);
  const auto b = splitLines(newText);
  const std::size_t n = a.size();
  const std::size_t m = b.size();
  if (n > 0 && m > 0 && n > kMaxDiffCells / m) {
    return "error: documents too large to diff here";
  }
  std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
  for (std::size_t i = n; i-- > 0;) {
    for (std::size_t j = m; j-- > 0;) {
      dp[i][j] = a[i] == b[j] ? dp[i + 1][j + 1] + 1 : std::max(dp[i + 1][j], dp[i][j + 1]);
    }
  }
  std::string out;
  int changes = 0;
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < n && j < m) {
    if (a[i] == b[j]) {
      out += "  ";
      out += a[i];
      out += '\n';
      ++i;
      ++j;
    } else if (dp[i + 1][j] >= dp[i][j + 1]) {
      out += "- ";
      out += a[i];
      out += '\n';
      ++i;
      ++changes;
    } else {
      out += "+ ";
      out += b[j];
      out += '\n';
      ++j;
      ++changes;
    }
    if (out.size() > kMaxDiffChars) {
      out += "... (diff truncated)\n";
      return out;
    }
  }
  while (i < n) {
    out += "- ";
    out += a[i];
    out += '\n';
    ++i;
    ++changes;
    if (out.size() > kMaxDiffChars) {
      out += "... (diff truncated)\n";
      return out;
    }
  }
  while (j < m) {
    out += "+ ";
    out += b[j];
    out += '\n';
    ++j;
    ++changes;
    if (out.size() > kMaxDiffChars) {
      out += "... (diff truncated)\n";
      return out;
    }
  }
  if (changes == 0) return "(no differences)\n";
  return out;
}

int64_t snapshotIdArg(const nlohmann::json& args) {
  if (!args.contains("snapshot_id")) return 0;
  const auto& v = args["snapshot_id"];
  if (v.is_number_integer()) {
    const auto n = v.get<int64_t>();
    return n > 0 ? n : 0;
  }
  if (v.is_string()) {
    const std::string raw = v.get<std::string>();
    if (raw.empty()) return 0;
    try {
      std::size_t consumed = 0;
      const long long n = std::stoll(raw, &consumed);
      if (consumed != raw.size() || n <= 0) return 0;
      return static_cast<int64_t>(n);
    } catch (const std::exception&) {
      return 0;
    }
  }
  return 0;
}

nlohmann::json eventsToJson(const std::vector<AgentEvent>& events) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& ev : events) {
    nlohmann::json data = ev.data;
    data.erase("streaming");
    arr.push_back(nlohmann::json{{"type", ev.type}, {"data", std::move(data)}});
  }
  return arr;
}

nlohmann::json messagesToJson(const std::vector<ChatMessage>& messages) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& msg : messages) {
    nlohmann::json row{{"role", msg.role}, {"content", msg.content}};
    if (!msg.toolCallId.empty()) row["toolCallId"] = msg.toolCallId;
    if (!msg.toolCalls.empty()) {
      nlohmann::json calls = nlohmann::json::array();
      for (const auto& c : msg.toolCalls) {
        calls.push_back(
            nlohmann::json{{"id", c.id}, {"name", c.name}, {"arguments", c.arguments}});
      }
      row["toolCalls"] = std::move(calls);
    }
    arr.push_back(std::move(row));
  }
  return arr;
}

std::vector<AgentEvent> eventsFromJson(const std::string& dumped) {
  std::vector<AgentEvent> out;
  if (dumped.empty()) return out;
  nlohmann::json arr = nlohmann::json::parse(dumped);
  if (!arr.is_array()) return out;
  for (const auto& row : arr) {
    AgentEvent ev;
    ev.type = row.value("type", std::string());
    if (row.contains("data") && row["data"].is_object()) ev.data = row["data"];
    else ev.data = nlohmann::json::object();
    out.push_back(std::move(ev));
  }
  return out;
}

std::vector<ChatMessage> messagesFromJson(const std::string& dumped) {
  std::vector<ChatMessage> out;
  if (dumped.empty()) return out;
  nlohmann::json arr = nlohmann::json::parse(dumped);
  if (!arr.is_array()) return out;
  for (const auto& row : arr) {
    ChatMessage msg;
    msg.role = row.value("role", std::string());
    msg.content = row.value("content", std::string());
    msg.toolCallId = row.value("toolCallId", std::string());
    if (row.contains("toolCalls") && row["toolCalls"].is_array()) {
      for (const auto& c : row["toolCalls"]) {
        ToolCall call;
        call.id = c.value("id", std::string());
        call.name = c.value("name", std::string());
        call.arguments = c.value("arguments", std::string());
        msg.toolCalls.push_back(std::move(call));
      }
    }
    out.push_back(std::move(msg));
  }
  return out;
}

}  // namespace

struct AgentRuntime::Session {
  std::string id;
  std::string kind = "draft";
  std::string status = "running";
  std::vector<AgentEvent> events;
  std::optional<AgentDraft> draft;
  std::vector<ChatMessage> messages;
  AgentDocumentSnapshot snapshot;
  AgentUiContext uiContext;
  std::string title;
  std::string createdAt;
  int getDocumentCount = 0;
  int queryBlockCount = 0;
  int historyDiffCount = 0;
  std::uint64_t gen = 0;
  std::atomic<bool> cancelled{false};
  std::thread worker;
};

AgentRuntime::AgentRuntime(index::FtsSearch& search, vault::DocumentService& documents,
                           index::NavQueries& nav, index::IndexUpdater& indexUpdater,
                           index::McpAuditLog* auditLog, ChatClient* chat,
                           std::string systemPrompt, std::string chatSystemPrompt,
                           index::AgentChatStore* chats, index::QueryBlocks* queryBlocks,
                           index::SnapshotStore* snapshots)
    : search_(search),
      documents_(documents),
      nav_(nav),
      indexUpdater_(indexUpdater),
      auditLog_(auditLog),
      chat_(chat),
      chatStore_(chats),
      queryBlocks_(queryBlocks),
      snapshots_(snapshots),
      systemPrompt_(resolveSystemPrompt(std::move(systemPrompt))),
      chatSystemPrompt_(resolveChatSystemPrompt(std::move(chatSystemPrompt))) {}

AgentRuntime::~AgentRuntime() {
  std::vector<std::shared_ptr<Session>> toJoin;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [id, session] : sessions_) {
      (void)id;
      session->cancelled.store(true);
      toJoin.push_back(session);
    }
    sessions_.clear();
  }
  if (chat_) chat_->cancel();
  for (auto& session : toJoin) {
    if (session->worker.joinable()) session->worker.join();
  }
}

nlohmann::json AgentRuntime::toolSchemas(const std::string& kind) {
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
         "Search the wiki (FTS5 plus semantic ranking when embeddings "
         "are enabled — the same engine as the site search). Returns "
         "matching paths, titles, tags, and a short snippet. Call this "
         "before answering or writing."},
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
       {{"name", "list_types"},
        {"description", "Every document type in use, with document counts."},
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
       {{"name", "run_query_block"},
        {"description",
         "Execute a wiki ```query fenced block (the live table the "
         "human sees on a page). Pass the block BODY only — the "
         "key: value lines (type, tag, folder, search, sort, order, "
         "limit, orphans), not the surrounding ``` fences. Same "
         "whitelisted DSL as GET /api/query; never raw SQL. Use this "
         "when get_document shows a query block and you need the "
         "matching documents, not the DSL itself."},
        {"parameters",
         {{"type", "object"},
          {"properties",
           {{"query", strProp("Query-block body, one key: value per line")}}},
          {"required", nlohmann::json::array({"query"})}}}}},
  });
  tools.push_back({
      {"type", "function"},
      {"function",
       {{"name", "list_document_history"},
        {"description",
         "Past snapshots of one document, newest first. Each row is "
         "id + snapshotAt. The live file is not in this list. Then "
         "diff_document_history with a snapshot id to see what "
         "changed versus current."},
        {"parameters",
         {{"type", "object"},
          {"properties", {{"path", strProp("Vault-relative document path")}}},
          {"required", nlohmann::json::array({"path"})}}}}},
  });
  tools.push_back({
      {"type", "function"},
      {"function",
       {{"name", "diff_document_history"},
        {"description",
         "Line diff of a past snapshot's body versus the current "
         "document (same as the History page). snapshot_id from "
         "list_document_history; omit it to diff the newest snapshot. "
         "Does not restore or write."},
        {"parameters",
         {{"type", "object"},
          {"properties",
           {{"path", strProp("Vault-relative document path")},
            {"snapshot_id",
             {{"type", "integer"},
              {"description",
               "Snapshot id from list_document_history; omit for the newest"}}}}},
          {"required", nlohmann::json::array({"path"})}}}}},
  });
  if (kind == "chat") {
    tools.push_back({
        {"type", "function"},
        {"function",
         {{"name", "get_current_view"},
          {"description",
           "The wiki page the human is looking at right now (document, "
           "folder, search, graph, editor, …), with vault path when there "
           "is one. Call this when they say 'this document', 'this "
           "folder', 'here', or 'the current page', then get_document or "
           "list_documents with that path. Do not ask which page if this "
           "returns a path."},
          {"parameters",
           {{"type", "object"}, {"properties", nlohmann::json::object()}}}}},
    });
    return tools;
  }
  tools.push_back({
      {"type", "function"},
      {"function",
       {{"name", "propose_draft"},
        {"description",
         "Replace the whole editor with this document. Use only for a new "
         "note or a requested full rewrite, and only after any needed "
         "clarifying questions have been answered. For a local change, use "
         "append_to_draft, insert_in_draft, or replace_in_draft instead. "
         "Does not save. "
         "Wiki-links in body must be literal [[path.md]] or "
         "[[path.md|Label]], never backslash-escaped."},
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
  tools.push_back({
      {"type", "function"},
      {"function",
       {{"name", "append_to_draft"},
        {"description",
         "Append markdown to the end of the current editor body. Use this "
         "for 'add a paragraph' / 'add a section at the end', once the "
         "request is clear. Does not save."},
        {"parameters",
         {{"type", "object"},
          {"properties", {{"text", strProp("Markdown to append")}}},
          {"required", nlohmann::json::array({"text"})}}}}},
  });
  tools.push_back({
      {"type", "function"},
      {"function",
       {{"name", "insert_in_draft"},
        {"description",
         "Insert markdown at the editor caret (no selection). Use this "
         "for 'insert a paragraph here' / 'add this above'. For the end "
         "of the document, append_to_draft is fine. Call only after the "
         "request is clear. Does not save."},
        {"parameters",
         {{"type", "object"},
          {"properties", {{"text", strProp("Markdown to insert at the caret")}}},
          {"required", nlohmann::json::array({"text"})}}}}},
  });
  tools.push_back({
      {"type", "function"},
      {"function",
       {{"name", "replace_in_draft"},
        {"description",
         "Replace one unique span in the current body. If the editor has a "
         "selection, omit find and that selection is used. find must match "
         "exactly once. Call only after the request is clear. Does not save."},
        {"parameters",
         {{"type", "object"},
          {"properties",
           {{"find", strProp("Exact text to replace; omit to use the editor selection")},
            {"replacement", strProp("Replacement markdown")}}},
          {"required", nlohmann::json::array({"replacement"})}}}}},
  });
  return tools;
}

std::string AgentRuntime::start(const std::string& instruction,
                                AgentDocumentSnapshot snapshot) {
  if (!chat_) throw std::runtime_error("agent not configured");
  if (instruction.empty()) throw std::runtime_error("instruction is required");

  auto session = std::make_shared<Session>();
  session->id = util::newUuidV4();
  session->kind = "draft";
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
    if (session->kind != "draft") {
      throw std::runtime_error("not a draft session");
    }
    for (const auto& [id, existing] : sessions_) {
      (void)id;
      if (existing.get() != session.get() && existing->status == "running") {
        throw std::runtime_error("an agent session is already running");
      }
    }
  }

  if (session->worker.joinable()) session->worker.join();

  session->snapshot = std::move(snapshot);
  session->status = "running";
  session->cancelled.store(false);
  session->getDocumentCount = 0;
  session->queryBlockCount = 0;
  session->historyDiffCount = 0;
  const std::string userText = snapshotUserPrefix(session->snapshot) + "\n\nInstruction:\n" +
                               instruction;
  session->messages.push_back(ChatMessage{"user", userText, "", {}});
  {
    std::lock_guard<std::mutex> lock(mu_);
    appendEventLocked(*session, AgentEvent{"user", nlohmann::json{{"text", instruction}}});
  }
  session->worker = std::thread([this, id = session->id]() { runLoop(id); });
}

std::string AgentRuntime::startChat(const std::string& instruction, AgentUiContext ui) {
  if (!chat_) throw std::runtime_error("agent not configured");
  if (instruction.empty()) throw std::runtime_error("instruction is required");

  auto session = std::make_shared<Session>();
  session->id = util::newUuidV4();
  session->kind = "chat";
  session->title = titleFromInstruction(instruction);
  session->createdAt = util::nowIso8601();
  session->uiContext = std::move(ui);
  session->messages.push_back(ChatMessage{"system", chatSystemPrompt_, "", {}});
  session->messages.push_back(
      ChatMessage{"user", chatUserText(session->uiContext, instruction), "", {}});
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

  persistChat(*session);
  session->worker = std::thread([this, id = session->id]() { runLoop(id); });
  return session->id;
}

void AgentRuntime::sendChat(const std::string& sessionId, const std::string& instruction,
                            AgentUiContext ui) {
  if (!chat_) throw std::runtime_error("agent not configured");
  if (instruction.empty()) throw std::runtime_error("instruction is required");
  loadChat(sessionId);

  std::shared_ptr<Session> session;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) throw std::runtime_error("session not found");
    session = it->second;
    if (session->status == "running") {
      throw std::runtime_error("session is still running");
    }
    if (session->kind != "chat") {
      throw std::runtime_error("not a chat session");
    }
    for (const auto& [id, existing] : sessions_) {
      (void)id;
      if (existing.get() != session.get() && existing->status == "running") {
        throw std::runtime_error("an agent session is already running");
      }
    }
  }

  if (session->worker.joinable()) session->worker.join();

  session->status = "running";
  session->cancelled.store(false);
  session->getDocumentCount = 0;
  session->queryBlockCount = 0;
  session->historyDiffCount = 0;
  session->uiContext = std::move(ui);
  session->messages.push_back(
      ChatMessage{"user", chatUserText(session->uiContext, instruction), "", {}});
  {
    std::lock_guard<std::mutex> lock(mu_);
    appendEventLocked(*session, AgentEvent{"user", nlohmann::json{{"text", instruction}}});
  }
  session->worker = std::thread([this, id = session->id]() { runLoop(id); });
}

std::optional<AgentSessionView> AgentRuntime::view(const std::string& sessionId) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sessions_.find(sessionId);
  if (it == sessions_.end()) return std::nullopt;
  AgentSessionView out;
  out.id = it->second->id;
  out.kind = it->second->kind;
  out.title = it->second->title;
  out.status = it->second->status;
  out.events = it->second->events;
  out.draft = it->second->draft;
  return out;
}

std::uint64_t AgentRuntime::generation(const std::string& sessionId) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sessions_.find(sessionId);
  if (it == sessions_.end()) return 0;
  return it->second->gen;
}

bool AgentRuntime::waitGeneration(const std::string& sessionId, std::uint64_t seen,
                                  std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(mu_);
  return cv_.wait_for(lock, timeout, [&] {
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) return true;
    return it->second->gen != seen;
  });
}

void AgentRuntime::appendEventLocked(Session& session, AgentEvent ev) {
  session.events.push_back(std::move(ev));
  session.gen++;
  cv_.notify_all();
}

void AgentRuntime::appendDeltaLocked(Session& session, std::string_view chunk) {
  if (chunk.empty()) return;
  if (!session.events.empty() && session.events.back().type == "assistant" &&
      session.events.back().data.value("streaming", false)) {
    auto text = session.events.back().data.value("text", std::string());
    text.append(chunk.data(), chunk.size());
    session.events.back().data["text"] = std::move(text);
    session.gen++;
    cv_.notify_all();
    return;
  }
  appendEventLocked(session, AgentEvent{
      "assistant",
      nlohmann::json{{"text", std::string(chunk)}, {"streaming", true}}});
}

void AgentRuntime::finishStreamingLocked(Session& session) {
  if (session.events.empty()) return;
  auto& last = session.events.back();
  if (last.type != "assistant") return;
  if (!last.data.value("streaming", false)) return;
  last.data.erase("streaming");
  session.gen++;
  cv_.notify_all();
}

void AgentRuntime::cancel(const std::string& sessionId) {
  std::shared_ptr<Session> session;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) throw std::runtime_error("session not found");
    session = it->second;
    session->cancelled.store(true);
    session->gen++;
    cv_.notify_all();
  }
  if (chat_) chat_->cancel();
}

void AgentRuntime::drop(const std::string& sessionId) {
  std::shared_ptr<Session> session;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
      session = it->second;
      session->cancelled.store(true);
      session->gen++;
      cv_.notify_all();
      sessions_.erase(it);
    }
  }
  if (session) {
    if (chat_) chat_->cancel();
    if (session->worker.joinable()) session->worker.join();
  }
  if (chatStore_) chatStore_->remove(sessionId);
}

void AgentRuntime::persistChat(const Session& session) {
  if (!chatStore_ || session.kind != "chat") return;
  index::AgentChatRecord row;
  {
    std::lock_guard<std::mutex> lock(mu_);
    row.id = session.id;
    row.title = session.title.empty() ? "Chat" : session.title;
    row.createdAt = session.createdAt.empty() ? util::nowIso8601() : session.createdAt;
    row.updatedAt = util::nowIso8601();
    row.eventsJson = eventsToJson(session.events).dump();
    row.messagesJson = messagesToJson(session.messages).dump();
  }
  chatStore_->upsert(row);
}

std::shared_ptr<AgentRuntime::Session> AgentRuntime::sessionFromRecord(
    const index::AgentChatRecord& row) const {
  auto session = std::make_shared<Session>();
  session->id = row.id;
  session->kind = "chat";
  session->title = row.title;
  session->createdAt = row.createdAt;
  session->status = "done";
  session->events = eventsFromJson(row.eventsJson);
  session->messages = messagesFromJson(row.messagesJson);
  if (!session->messages.empty() && session->messages.front().role == "system") {
    session->messages.front().content = chatSystemPrompt_;
  }
  return session;
}

bool AgentRuntime::loadChat(const std::string& sessionId) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (sessions_.count(sessionId)) return true;
  }
  if (!chatStore_) return false;
  auto rec = chatStore_->get(sessionId);
  if (!rec) return false;
  auto session = sessionFromRecord(*rec);
  std::lock_guard<std::mutex> lock(mu_);
  if (sessions_.count(sessionId)) return true;
  sessions_[sessionId] = std::move(session);
  return true;
}

std::vector<index::AgentChatSummary> AgentRuntime::listChats() const {
  if (!chatStore_) return {};
  return chatStore_->list();
}

void AgentRuntime::renameChat(const std::string& sessionId, const std::string& title) {
  const std::string trimmed = trimCopy(title);
  if (trimmed.empty()) throw std::runtime_error("title is required");
  if (chatStore_) chatStore_->rename(sessionId, trimmed);
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sessions_.find(sessionId);
  if (it != sessions_.end()) it->second->title = trimmed;
  else if (!chatStore_) throw std::runtime_error("session not found");
}

void AgentRuntime::runLoop(const std::string& sessionId) {
  std::shared_ptr<Session> session;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) return;
    session = it->second;
  }

  struct PersistOnExit {
    AgentRuntime* runtime;
    Session* session;
    ~PersistOnExit() {
      try {
        runtime->persistChat(*session);
      } catch (...) {
      }
    }
  };
  PersistOnExit persistGuard{this, session.get()};

  auto fail = [&](const std::string& message) {
    std::lock_guard<std::mutex> lock(mu_);
    session->status = "error";
    appendEventLocked(*session, AgentEvent{"error", nlohmann::json{{"text", message}}});
  };

  auto finishCancelled = [&]() {
    std::lock_guard<std::mutex> lock(mu_);
    if (session->status == "running") {
      session->status = "cancelled";
      appendEventLocked(*session, AgentEvent{"cancelled", nlohmann::json::object()});
    }
  };

  try {
    const nlohmann::json tools = toolSchemas(session->kind);
    bool finished = false;
    for (int step = 0; step < kMaxSteps && !finished; ++step) {
      if (session->cancelled.load()) {
        finishCancelled();
        return;
      }
      std::vector<ChatMessage> messages;
      {
        std::lock_guard<std::mutex> lock(mu_);
        messages = session->messages;
      }
      const ChatCompletion completion = chat_->complete(
          messages, tools, [&](std::string_view chunk) {
            std::lock_guard<std::mutex> lock(mu_);
            if (session->cancelled.load()) return;
            appendDeltaLocked(*session, chunk);
          });
      if (session->cancelled.load()) {
        finishCancelled();
        return;
      }

      if (!completion.toolCalls.empty()) {
        ChatMessage assistant;
        assistant.role = "assistant";
        assistant.content = completion.content;
        assistant.toolCalls = completion.toolCalls;
        {
          std::lock_guard<std::mutex> lock(mu_);
          finishStreamingLocked(*session);
          session->messages.push_back(assistant);
        }
        for (const auto& call : completion.toolCalls) {
          if (session->cancelled.load()) {
            finishCancelled();
            return;
          }
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
          const bool wrote =
              call.name == "propose_draft" || call.name == "append_to_draft" ||
              call.name == "insert_in_draft" || call.name == "replace_in_draft";
          if (wrote && session->draft && result.rfind("error:", 0) != 0) {
            finished = true;
          }
        }
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(mu_);
        session->messages.push_back(
            ChatMessage{"assistant", completion.content, "", {}});
        if (!session->events.empty() && session->events.back().type == "assistant" &&
            session->events.back().data.value("streaming", false)) {
          if (!completion.content.empty()) {
            session->events.back().data["text"] = completion.content;
          }
          finishStreamingLocked(*session);
        } else if (!completion.content.empty()) {
          appendEventLocked(*session, AgentEvent{"assistant", nlohmann::json{
                                                                  {"text", completion.content}}});
        }
      }
      finished = true;
    }

    if (session->cancelled.load()) {
      finishCancelled();
      return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (session->status == "running") {
      session->status = "done";
      appendEventLocked(*session, AgentEvent{"done", nlohmann::json::object()});
    }
  } catch (const std::exception& e) {
    if (session->cancelled.load()) {
      finishCancelled();
    } else {
      fail(e.what());
    }
  }
}

std::string AgentRuntime::executeTool(Session& session, const std::string& name,
                                      const nlohmann::json& args) {
  auto audit = [&](const std::string& tool, const std::string& path, bool ok,
                   const std::string& detail) {
    if (!auditLog_) return;
    const char* prefix = session.kind == "chat" ? "chat:" : "compose:";
    auditLog_->record(std::string(prefix) + tool, path, ok, detail);
  };

  if (session.kind == "chat" &&
      (name == "propose_draft" || name == "append_to_draft" ||
       name == "insert_in_draft" || name == "replace_in_draft")) {
    throw std::runtime_error("write tools are not available in chat");
  }

  if (name == "get_current_view") {
    if (session.kind != "chat") {
      throw std::runtime_error("get_current_view is only available in chat");
    }
    const nlohmann::json out = uiContextJson(session.uiContext);
    {
      std::lock_guard<std::mutex> lock(mu_);
      appendEventLocked(session, AgentEvent{
          "tool", nlohmann::json{{"name", name},
                                 {"detail", session.uiContext.path}}});
    }
    audit(name, session.uiContext.path, true, session.uiContext.page);
    return out.dump(2);
  }

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
      appendEventLocked(session, 
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
      appendEventLocked(session, 
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
      appendEventLocked(session, AgentEvent{
          "tool", nlohmann::json{{"name", name}, {"count", static_cast<int>(tags.size())}}});
    }
    audit(name, "", true, "");
    return arr.dump(2);
  }

  if (name == "list_types") {
    const auto types = nav_.typeCounts(true);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& t : types) {
      arr.push_back(nlohmann::json{{"type", t.tag}, {"count", t.count}});
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      appendEventLocked(session, AgentEvent{
          "tool", nlohmann::json{{"name", name}, {"count", static_cast<int>(types.size())}}});
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
      appendEventLocked(session, AgentEvent{
          "tool", nlohmann::json{{"name", name}, {"count", static_cast<int>(results.size())}}});
    }
    audit(name, q.folderPrefix.value_or(""), true, "");
    return arr.dump(2);
  }

  if (name == "run_query_block") {
    if (!queryBlocks_) throw std::runtime_error("query blocks not available");
    if (session.queryBlockCount >= kMaxQueryBlocks) {
      throw std::runtime_error("run_query_block limit reached for this turn");
    }
    if (!args.contains("query") || !args["query"].is_string()) {
      throw std::runtime_error("run_query_block requires query");
    }
    const std::string raw = stripQueryFence(args["query"].get<std::string>());
    const auto result = queryBlocks_->parseAndRun(raw, true);
    session.queryBlockCount++;
    if (!result.ok) {
      audit(name, "", false, result.error);
      throw std::runtime_error(result.error);
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& row : result.rows) {
      arr.push_back(nlohmann::json{{"path", row.path},
                                   {"title", row.title},
                                   {"visibility", row.visibility},
                                   {"updatedAt", row.updatedAt},
                                   {"tags", row.tagsFlat}});
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      appendEventLocked(session, AgentEvent{
          "tool", nlohmann::json{{"name", name},
                                 {"count", static_cast<int>(result.rows.size())}}});
    }
    audit(name, "", true, raw.substr(0, 80));
    return arr.dump(2);
  }

  if (name == "list_document_history") {
    if (!snapshots_) throw std::runtime_error("document history not available");
    if (!args.contains("path") || !args["path"].is_string() ||
        args["path"].get<std::string>().empty()) {
      throw std::runtime_error("list_document_history requires path");
    }
    const std::string path = unescapeModelMarkdown(args["path"].get<std::string>());
    try {
      (void)documents_.get(path);
    } catch (const vault::DocumentNotFoundError&) {
      audit(name, path, false, "not found");
      throw std::runtime_error("document not found: " + path);
    } catch (const vault::PathTraversalError&) {
      audit(name, path, false, "invalid path");
      throw std::runtime_error("invalid path");
    }
    const auto rowId = indexUpdater_.rowIdForPath(path);
    if (!rowId) {
      audit(name, path, false, "not indexed");
      throw std::runtime_error("document not indexed");
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : snapshots_->list(*rowId)) {
      arr.push_back(nlohmann::json{{"id", s.id}, {"snapshotAt", s.snapshotAt}});
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      appendEventLocked(session, AgentEvent{
          "tool", nlohmann::json{{"name", name},
                                 {"detail", path},
                                 {"count", static_cast<int>(arr.size())}}});
    }
    audit(name, path, true, "");
    nlohmann::json out;
    out["path"] = path;
    out["snapshots"] = std::move(arr);
    out["note"] = "Live content is not listed. Diff a snapshot id against current "
                  "with diff_document_history.";
    return out.dump(2);
  }

  if (name == "diff_document_history") {
    if (!snapshots_) throw std::runtime_error("document history not available");
    if (session.historyDiffCount >= kMaxHistoryDiffs) {
      throw std::runtime_error("diff_document_history limit reached for this turn");
    }
    if (!args.contains("path") || !args["path"].is_string() ||
        args["path"].get<std::string>().empty()) {
      throw std::runtime_error("diff_document_history requires path");
    }
    const std::string path = unescapeModelMarkdown(args["path"].get<std::string>());
    vault::DocumentRecord live;
    try {
      live = documents_.get(path);
    } catch (const vault::DocumentNotFoundError&) {
      audit(name, path, false, "not found");
      throw std::runtime_error("document not found: " + path);
    } catch (const vault::PathTraversalError&) {
      audit(name, path, false, "invalid path");
      throw std::runtime_error("invalid path");
    }
    const auto rowId = indexUpdater_.rowIdForPath(path);
    if (!rowId) {
      audit(name, path, false, "not indexed");
      throw std::runtime_error("document not indexed");
    }
    const auto listed = snapshots_->list(*rowId);
    if (listed.empty()) {
      audit(name, path, true, "no snapshots");
      return "No past versions yet — history starts recording from the next edit.";
    }
    int64_t snapId = snapshotIdArg(args);
    if (snapId == 0) snapId = listed.front().id;
    const auto content = snapshots_->getContent(*rowId, snapId);
    if (!content) {
      audit(name, path, false, "no such snapshot");
      throw std::runtime_error("no such snapshot for this document");
    }
    const auto parsed = vault::parseFrontMatter(*content);
    session.historyDiffCount++;
    std::string at;
    for (const auto& s : listed) {
      if (s.id == snapId) {
        at = s.snapshotAt;
        break;
      }
    }
    std::string out = "--- snapshot " + std::to_string(snapId);
    if (!at.empty()) {
      out += " (";
      out += at;
      out += ")";
    }
    out += "\n+++ current\n";
    out += lineDiff(parsed.body, live.body);
    {
      std::lock_guard<std::mutex> lock(mu_);
      appendEventLocked(session, AgentEvent{
          "tool", nlohmann::json{{"name", name},
                                 {"detail", path},
                                 {"snapshotId", snapId}}});
    }
    audit(name, path, true, std::to_string(snapId));
    return out;
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
      appendEventLocked(session, AgentEvent{
          "draft", nlohmann::json{{"path", draft.path}, {"title", draft.title}}});
    }
    audit(name, draft.path, true, draft.title);
    return "Draft recorded for the editor. Stop. Do not claim the file was saved.";
  }

  if (name == "append_to_draft") {
    if (!args.contains("text") || !args["text"].is_string() ||
        args["text"].get<std::string>().empty()) {
      throw std::runtime_error("append_to_draft requires non-empty text");
    }
    const std::string extra = unescapeModelMarkdown(args["text"].get<std::string>());
    std::string path;
    {
      std::lock_guard<std::mutex> lock(mu_);
      ensureDraftFromSnapshot(session.draft, session.snapshot);
      appendBlock(session.draft->body, extra);
      path = session.draft->path;
      appendEventLocked(session, 
          AgentEvent{"edit", nlohmann::json{{"op", "append"}, {"text", extra}}});
    }
    audit(name, path, true, extra.substr(0, 80));
    return "Appended to the editor. Stop. Do not claim the file was saved.";
  }

  if (name == "insert_in_draft") {
    if (!args.contains("text") || !args["text"].is_string() ||
        args["text"].get<std::string>().empty()) {
      throw std::runtime_error("insert_in_draft requires non-empty text");
    }
    if (!session.snapshot.caretBefore) {
      throw std::runtime_error(
          "insert_in_draft needs the editor caret (no selection). Put the "
          "caret where the text should go, or use append_to_draft / "
          "replace_in_draft");
    }
    const std::string extra = unescapeModelMarkdown(args["text"].get<std::string>());
    const std::string after = *session.snapshot.caretBefore;
    std::string path;
    {
      std::lock_guard<std::mutex> lock(mu_);
      ensureDraftFromSnapshot(session.draft, session.snapshot);
      const std::size_t pos = insertOffset(session.draft->body, after);
      session.draft->body.insert(pos, extra);
      path = session.draft->path;
      appendEventLocked(session, AgentEvent{
          "edit", nlohmann::json{{"op", "insert"}, {"after", after}, {"text", extra}}});
    }
    audit(name, path, true, extra.substr(0, 80));
    return "Inserted at the caret. Stop. Do not claim the file was saved.";
  }

  if (name == "replace_in_draft") {
    if (!args.contains("replacement") || !args["replacement"].is_string()) {
      throw std::runtime_error("replace_in_draft requires replacement");
    }
    std::string find;
    if (args.contains("find") && args["find"].is_string()) {
      find = unescapeModelMarkdown(args["find"].get<std::string>());
    }
    if (find.empty()) find = session.snapshot.selection;
    if (find.empty()) {
      throw std::runtime_error(
          "replace_in_draft needs find, or a non-empty editor selection");
    }
    const std::string replacement =
        unescapeModelMarkdown(args["replacement"].get<std::string>());
    std::string path;
    {
      std::lock_guard<std::mutex> lock(mu_);
      ensureDraftFromSnapshot(session.draft, session.snapshot);
      const std::size_t hits = countOccurrences(session.draft->body, find);
      if (hits == 0) {
        throw std::runtime_error("find text not found in the current body");
      }
      if (hits > 1) {
        throw std::runtime_error("find text matches " + std::to_string(hits) +
                                 " times — include more surrounding context");
      }
      const auto pos = session.draft->body.find(find);
      session.draft->body.replace(pos, find.size(), replacement);
      path = session.draft->path;
      appendEventLocked(session, AgentEvent{
          "edit",
          nlohmann::json{{"op", "replace"}, {"find", find}, {"replacement", replacement}}});
    }
    audit(name, path, true, find.substr(0, 80));
    return "Replaced the span in the editor. Stop. Do not claim the file was saved.";
  }

  throw std::runtime_error("unknown tool: " + name);
}

}  // namespace wikicore::llm
