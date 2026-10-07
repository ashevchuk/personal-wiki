#include "mcp/McpServer.h"

#include "embeddings/EmbeddingProviderFactory.h"
#include "util/Base64.h"
#include "vault/AttachToDocument.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace wikicore;

namespace wikicore::mcp {

// Hand-rolled JSON-RPC 2.0 / MCP stdio transport. A JSON-RPC notification
// (a message with no "id", e.g. notifications/initialized) must never
// receive a response, full stop — the loop below enforces that
// generically (no response is written whenever the incoming message has
// no "id"), not as a special case keyed on one method name.
//
// `json` is nlohmann::ordered_json from the project's own vcpkg
// nlohmann_json dependency (already linked into wikicore) — no separate
// vendored copy. Tool schemas come out in registration order rather
// than key-sorted; MCP clients don't care about object key order either
// way, so this is a readability nicety, not a behavioral requirement.
using json = nlohmann::ordered_json;

enum class error_code : int {
  parse_error = -32700,
  invalid_request = -32600,
  method_not_found = -32601,
  invalid_params = -32602,
  internal_error = -32603,
};

class mcp_exception : public std::runtime_error {
 public:
  mcp_exception(error_code code, const std::string& message)
      : std::runtime_error(message), code_(code) {}
  error_code code() const { return code_; }

 private:
  error_code code_;
};

using tool_handler = std::function<json(const json& params)>;

struct tool {
  std::string name;
  std::string description;
  json inputSchema;

  json toJson() const {
    return json{{"name", name}, {"description", description}, {"inputSchema", inputSchema}};
  }
};

// Fluent builder covering only the parameter kinds this project's tools
// actually use (string, number, string-array) — no boolean/object
// params, so none are implemented.
class tool_builder {
 public:
  explicit tool_builder(std::string name) : name_(std::move(name)) {
    schema_["type"] = "object";
    schema_["properties"] = json::object();
  }

  tool_builder& with_description(std::string description) {
    description_ = std::move(description);
    return *this;
  }
  tool_builder& with_string_param(const std::string& name, const std::string& description,
                                   bool required) {
    return addParam(name, json{{"type", "string"}, {"description", description}}, required);
  }
  tool_builder& with_number_param(const std::string& name, const std::string& description,
                                   bool required) {
    return addParam(name, json{{"type", "number"}, {"description", description}}, required);
  }
  tool_builder& with_array_param(const std::string& name, const std::string& description,
                                  const std::string& itemType, bool required) {
    return addParam(name,
                     json{{"type", "array"},
                          {"description", description},
                          {"items", json{{"type", itemType}}}},
                     required);
  }

  tool build() const {
    json schema = schema_;
    if (!required_.empty()) schema["required"] = required_;
    return tool{name_, description_, schema};
  }

 private:
  tool_builder& addParam(const std::string& name, json paramSchema, bool required) {
    schema_["properties"][name] = std::move(paramSchema);
    if (required) required_.push_back(name);
    return *this;
  }

  std::string name_;
  std::string description_;
  json schema_;
  std::vector<std::string> required_;
};

// One process serves exactly one client for its whole lifetime (stdio,
// spawned fresh per MCP session) — no multi-session bookkeeping, no
// transport abstraction.
class server {
 public:
  server(std::string name, std::string version)
      : name_(std::move(name)), version_(std::move(version)) {}

  void register_tool(tool t, tool_handler handler) {
    tools_.push_back({std::move(t), std::move(handler)});
  }

  // Blocks until stdin closes. One JSON-RPC message per line.
  void start_stdio();

 private:
  json dispatch(const std::string& method, const json& params);
  json handleInitialize() const;
  json handleToolsCall(const json& params);

  std::string name_;
  std::string version_;
  struct ToolEntry {
    tool def;
    tool_handler handler;
  };
  std::vector<ToolEntry> tools_;
};

json server::handleInitialize() const {
  return json{{"protocolVersion", "2025-03-26"},
              {"capabilities", json{{"tools", json::object()}}},
              {"serverInfo", json{{"name", name_}, {"version", version_}}}};
}

json server::handleToolsCall(const json& params) {
  if (!params.contains("name") || !params["name"].is_string()) {
    throw mcp_exception(error_code::invalid_params, "missing 'name'");
  }
  const std::string toolName = params["name"].get<std::string>();
  const auto it = std::find_if(tools_.begin(), tools_.end(),
                                [&](const ToolEntry& e) { return e.def.name == toolName; });
  if (it == tools_.end()) {
    throw mcp_exception(error_code::invalid_params, "Tool not found: " + toolName);
  }
  const json args = params.value("arguments", json::object());

  // A handler-thrown exception (including mcp_exception) becomes a
  // successful JSON-RPC response carrying isError:true, per the MCP
  // spec's own distinction between a tool-execution failure and a
  // protocol-level error — only "missing name"/"tool not found" above,
  // thrown before this try, surface as real JSON-RPC errors.
  json result;
  result["isError"] = false;
  try {
    result["content"] = it->handler(args);
  } catch (const std::exception& e) {
    result["isError"] = true;
    result["content"] = json::array({json{{"type", "text"}, {"text", e.what()}}});
  }
  return result;
}

json server::dispatch(const std::string& method, const json& params) {
  if (method == "initialize") return handleInitialize();
  if (method == "ping") return json::object();
  if (method == "tools/list") {
    json arr = json::array();
    for (const auto& entry : tools_) arr.push_back(entry.def.toJson());
    return json{{"tools", arr}};
  }
  if (method == "tools/call") return handleToolsCall(params);
  throw mcp_exception(error_code::method_not_found, "Method not found: " + method);
}

void server::start_stdio() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;

    json request;
    try {
      request = json::parse(line);
    } catch (const json::parse_error& e) {
      std::cout << json{{"jsonrpc", "2.0"},
                         {"id", nullptr},
                         {"error", json{{"code", static_cast<int>(error_code::parse_error)},
                                        {"message", std::string("parse error: ") + e.what()}}}}
                       .dump()
                << "\n"
                << std::flush;
      continue;
    }
    if (!request.is_object() || request.value("jsonrpc", std::string()) != "2.0") {
      std::cout << json{{"jsonrpc", "2.0"},
                         {"id", nullptr},
                         {"error", json{{"code", static_cast<int>(error_code::invalid_request)},
                                        {"message", "invalid JSON-RPC envelope"}}}}
                       .dump()
                << "\n"
                << std::flush;
      continue;
    }

    // A request with no "id" is a JSON-RPC notification: it must never
    // get a response, not even an empty one.
    const bool isNotification = !request.contains("id");
    const json id = isNotification ? json(nullptr) : request["id"];
    const std::string method = request.value("method", std::string());
    const json params = request.value("params", json::object());

    json result;
    try {
      result = dispatch(method, params);
    } catch (const mcp_exception& e) {
      if (!isNotification) {
        std::cout << json{{"jsonrpc", "2.0"},
                           {"id", id},
                           {"error", json{{"code", static_cast<int>(e.code())},
                                          {"message", e.what()}}}}
                         .dump()
                  << "\n"
                  << std::flush;
      }
      continue;
    } catch (const std::exception& e) {
      if (!isNotification) {
        std::cout << json{{"jsonrpc", "2.0"},
                           {"id", id},
                           {"error", json{{"code", static_cast<int>(error_code::internal_error)},
                                          {"message", e.what()}}}}
                         .dump()
                  << "\n"
                  << std::flush;
      }
      continue;
    }

    if (isNotification) continue;
    std::cout << json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}.dump() << "\n"
              << std::flush;
  }
}

namespace {

// Constructs a real EmbeddingProvider only on the first actual write —
// see McpServer.h's own comment on `cfg` for why this matters for
// wiki-mcp specifically (fast-starting, spawned per session, most
// sessions never write at all). NOT thread-safe, but doesn't need to be:
// srv.start_stdio() processes one request at a time.
class LazyEmbeddingProvider {
 public:
  LazyEmbeddingProvider(index::IndexUpdater& indexUpdater, const config::AppConfig& cfg)
      : indexUpdater_(indexUpdater), cfg_(cfg) {}

  // Idempotent: a no-op on every call after the first (successful or
  // failed) attempt.
  void ensureLoaded() {
    if (attempted_) return;
    attempted_ = true;
    try {
      provider_ = embeddings::createEmbeddingProvider(cfg_);
      indexUpdater_.setProvider(provider_.get());
    } catch (const std::exception& e) {
      // Best-effort, same philosophy as IndexUpdater::upsertOne's own
      // embedding step — a broken embeddings config must never block the
      // actual document write, which FTS5 already serves regardless.
      // Logged once (attempted_ prevents retrying every subsequent
      // write), to stderr only — see runServer()'s own comment on why
      // stdout is reserved exclusively for JSON-RPC framing.
      std::fprintf(stderr,
                    "wiki-mcp: failed to initialize embeddings.provider = \"%s\" on first "
                    "write — writes will stay FTS5-only this session: %s\n",
                    cfg_.embeddingsProvider.c_str(), e.what());
    }
  }

 private:
  index::IndexUpdater& indexUpdater_;
  const config::AppConfig& cfg_;
  std::unique_ptr<embeddings::EmbeddingProvider> provider_;
  bool attempted_ = false;
};

// FtsSearch's snippet() highlight markers (see docs/architecture.md) are
// control bytes meant for an HTML render step to escape-then-swap into
// <mark> tags. An MCP tool result isn't HTML — it's read directly by an
// LLM — so here they become plain markdown bold instead, which is legible
// content rather than raw control bytes leaking into the model's context.
std::string renderMcpSnippet(const index::SearchResultItem& item) {
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

json searchResultToJson(const index::SearchResultItem& item) {
  return json{
      {"path", item.path},       {"title", item.title},
      {"visibility", item.visibility}, {"type", item.docType},
      {"tags", item.tags},       {"updated", item.updatedAt},
      {"snippet", renderMcpSnippet(item)},
  };
}

json textContent(const std::string& text) {
  return json::array({json{{"type", "text"}, {"text", text}}});
}

// Applies the fail-safe-private rule independently at the tool layer, on
// top of whatever `includePrivate` already threaded through the query —
// the same category of check that got missed on the HTTP mutating routes
// in M2 (see docs/architecture.md's postmortem) doesn't get skipped here.
bool isVisibleTo(const std::string& visibility, bool includePrivate) {
  return includePrivate || visibility == "public";
}

tool_handler makeSearchDocumentsHandler(index::FtsSearch& search,
                                                bool includePrivate) {
  return [&search, includePrivate](const json& params) -> json {
    if (!params.contains("query") || !params["query"].is_string() ||
        params["query"].get<std::string>().empty()) {
      throw mcp_exception(error_code::invalid_params,
                                  "missing or empty 'query'");
    }

    index::SearchQuery q;
    q.text = params["query"].get<std::string>();
    q.includePrivate = includePrivate;
    q.limit = params.value("limit", 20);
    if (params.contains("type") && params["type"].is_string()) {
      q.docType = params["type"].get<std::string>();
    }
    if (params.contains("tags") && params["tags"].is_array()) {
      q.tags = params["tags"].get<std::vector<std::string>>();
    }

    const auto results = search.search(q);
    json arr = json::array();
    for (const auto& item : results) arr.push_back(searchResultToJson(item));
    return textContent(arr.dump(2));
  };
}

tool_handler makeGetDocumentHandler(vault::DocumentService& documents,
                                            index::IndexUpdater& indexUpdater,
                                            bool includePrivate) {
  return [&documents, &indexUpdater, includePrivate](
             const json& params) -> json {
    if (!params.contains("id_or_path") || !params["id_or_path"].is_string()) {
      throw mcp_exception(error_code::invalid_params,
                                  "missing 'id_or_path'");
    }
    const std::string idOrPath = params["id_or_path"].get<std::string>();

    auto tryGet = [&](const std::string& path) -> std::optional<vault::DocumentRecord> {
      try {
        return documents.get(path);
      } catch (const vault::DocumentNotFoundError&) {
        return std::nullopt;
      } catch (const vault::PathTraversalError&) {
        return std::nullopt;
      }
    };

    auto record = tryGet(idOrPath);
    if (!record) {
      if (const auto resolvedPath = indexUpdater.findPathByUuid(idOrPath)) {
        record = tryGet(*resolvedPath);
      }
    }
    if (!record || !isVisibleTo(record->frontMatter.visibility, includePrivate)) {
      throw mcp_exception(error_code::invalid_params,
                                  "document not found: " + idOrPath);
    }

    std::string out;
    out += "Path: " + record->path + "\n";
    out += "Title: " + record->frontMatter.title + "\n";
    out += "Tags: ";
    for (size_t i = 0; i < record->frontMatter.tags.size(); ++i) {
      if (i) out += ", ";
      out += record->frontMatter.tags[i];
    }
    out += "\n";
    out += "Visibility: " + record->frontMatter.visibility + "\n";
    out += "Type: " + record->frontMatter.type + "\n";
    out += "Updated: " + record->frontMatter.updated + "\n\n---\n\n";
    out += record->body;
    return textContent(out);
  };
}

tool_handler makeListTagsHandler(index::NavQueries& nav, bool includePrivate) {
  return [&nav, includePrivate](const json&) -> json {
    const auto tags = nav.tagCounts(includePrivate);
    json arr = json::array();
    for (const auto& t : tags) {
      arr.push_back(json{{"tag", t.tag}, {"count", t.count}});
    }
    return textContent(arr.dump(2));
  };
}

tool_handler makeListDocumentsHandler(index::FtsSearch& search,
                                              bool includePrivate) {
  return [&search, includePrivate](const json& params) -> json {
    index::SearchQuery q;
    q.includePrivate = includePrivate;
    q.limit = params.value("limit", 50);
    q.offset = params.value("offset", 0);
    if (params.contains("tag") && params["tag"].is_string()) {
      q.tag = params["tag"].get<std::string>();
    }
    if (params.contains("type") && params["type"].is_string()) {
      q.docType = params["type"].get<std::string>();
    }
    if (params.contains("folder") && params["folder"].is_string()) {
      q.folderPrefix = params["folder"].get<std::string>();
    }

    const auto results = search.search(q);
    json arr = json::array();
    for (const auto& item : results) {
      arr.push_back(json{
          {"path", item.path},       {"title", item.title},
          {"visibility", item.visibility}, {"type", item.docType},
          {"tags", item.tags},       {"updated", item.updatedAt},
          {"excerpt", item.snippet},  // browse mode: plain excerpt, no markers
      });
    }
    return textContent(arr.dump(2));
  };
}

// Same whitelisted DSL as GET /api/query and the Draft/Chat agent's own
// run_query_block tool (AgentRuntime.cpp) — one `key: value` grammar,
// one implementation (QueryBlocks::parseAndRun), exposed identically
// across every caller instead of three parallel reimplementations.
tool_handler makeRunQueryHandler(index::QueryBlocks& queryBlocks,
                                         bool includePrivate) {
  return [&queryBlocks, includePrivate](const json& params) -> json {
    if (!params.contains("query") || !params["query"].is_string()) {
      throw mcp_exception(error_code::invalid_params,
                                  "missing 'query'");
    }
    const auto result =
        queryBlocks.parseAndRun(params["query"].get<std::string>(), includePrivate);
    if (!result.ok) {
      // Same discipline as a typo'd query-block in a document: a parse
      // error must surface as an actual tool error, never as "ran fine,
      // zero rows" — see QueryBlocks.h's own comment on why.
      throw mcp_exception(error_code::invalid_params, result.error);
    }
    json arr = json::array();
    for (const auto& row : result.rows) {
      arr.push_back(json{{"path", row.path},
                                 {"title", row.title},
                                 {"visibility", row.visibility},
                                 {"updatedAt", row.updatedAt},
                                 {"tags", row.tagsFlat}});
    }
    return textContent(arr.dump(2));
  };
}

// Same engine as /calendar and GET /api/calendar (CalendarQueries.cpp) —
// recurring series already expanded into concrete per-day occurrences,
// one row per occurrence.
tool_handler makeGetCalendarEventsHandler(index::CalendarQueries& calendarQueries,
                                                  bool includePrivate) {
  return [&calendarQueries, includePrivate](const json& params) -> json {
    if (!params.contains("start") || !params["start"].is_string() ||
        !params.contains("end") || !params["end"].is_string()) {
      throw mcp_exception(error_code::invalid_params,
                                  "missing 'start' or 'end'");
    }
    const std::string folder =
        params.contains("folder") && params["folder"].is_string()
            ? params["folder"].get<std::string>()
            : "";
    std::vector<std::string> tags;
    if (params.contains("tags") && params["tags"].is_array()) {
      tags = params["tags"].get<std::vector<std::string>>();
    }
    const auto events = calendarQueries.eventsBetween(
        params["start"].get<std::string>(), params["end"].get<std::string>(),
        includePrivate, folder, tags);
    json arr = json::array();
    for (const auto& ev : events) {
      arr.push_back(json{{"path", ev.path},
                                 {"title", ev.title},
                                 {"visibility", ev.visibility},
                                 {"date", ev.date},
                                 {"time", ev.time}});
    }
    return textContent(arr.dump(2));
  };
}

// A path an LLM client hands us is exactly as untrusted as one from an
// anonymous HTTP request — DocumentService/VaultRepository already
// reject traversal (PathTraversalError), this just makes sure the
// attempt still lands in the audit log rather than only ever showing up
// as a generic error the caller sees but the admin never does.
tool_handler makeCreateDocumentHandler(vault::DocumentService& documents,
                                               index::McpAuditLog& auditLog,
                                               LazyEmbeddingProvider& lazyProvider) {
  return [&documents, &auditLog, &lazyProvider](const json& params) -> json {
    if (!params.contains("path") || !params["path"].is_string() ||
        params["path"].get<std::string>().empty()) {
      throw mcp_exception(error_code::invalid_params, "missing 'path'");
    }
    const std::string path = params["path"].get<std::string>();
    lazyProvider.ensureLoaded();

    vault::DocumentInput input;
    input.title = params.value("title", std::string());
    input.body = params.value("body", std::string());
    input.type = params.value("type", std::string());
    input.due = params.value("due", std::string());
    input.recur = params.value("recur", std::string());
    // Fail-safe-private applies here exactly as it does to a
    // human-authored save through the HTTP API — an LLM omitting
    // "visibility" (or getting the exact string wrong) must not
    // accidentally publish something.
    input.visibility = params.value("visibility", std::string("private"));
    if (params.contains("tags") && params["tags"].is_array()) {
      input.tags = params["tags"].get<std::vector<std::string>>();
    }

    try {
      const auto rec = documents.create(path, input);
      auditLog.record("create_document", rec.path, true, "created");
      return textContent("Created " + rec.path);
    } catch (const vault::DocumentAlreadyExistsError&) {
      auditLog.record("create_document", path, false, "a document already exists at that path");
      throw mcp_exception(error_code::invalid_params,
                                  "a document already exists at that path");
    } catch (const vault::PathTraversalError&) {
      auditLog.record("create_document", path, false, "path traversal rejected");
      throw mcp_exception(error_code::invalid_params, "invalid path");
    } catch (const std::exception& e) {
      auditLog.record("create_document", path, false, e.what());
      throw mcp_exception(error_code::internal_error, e.what());
    }
  };
}

// Merges onto the EXISTING document rather than requiring every field —
// DocumentService::update() itself has no concept of a partial update
// (it always writes a complete DocumentInput, same as the HTTP PUT
// route), so an LLM caller that only means to change the body shouldn't
// have to first fetch and echo back the title/tags/type/visibility it
// isn't touching.
tool_handler makeUpdateDocumentHandler(vault::DocumentService& documents,
                                               index::McpAuditLog& auditLog,
                                               LazyEmbeddingProvider& lazyProvider) {
  return [&documents, &auditLog, &lazyProvider](const json& params) -> json {
    if (!params.contains("path") || !params["path"].is_string() ||
        params["path"].get<std::string>().empty()) {
      throw mcp_exception(error_code::invalid_params, "missing 'path'");
    }
    const std::string path = params["path"].get<std::string>();
    lazyProvider.ensureLoaded();

    try {
      const vault::DocumentRecord existing = documents.get(path);

      vault::DocumentInput input;
      input.title = params.value("title", existing.frontMatter.title);
      input.body = params.value("body", existing.body);
      input.type = params.value("type", existing.frontMatter.type);
      input.due = params.value("due", existing.frontMatter.due);
      input.recur = params.value("recur", existing.frontMatter.recur);
      input.visibility = params.value("visibility", existing.frontMatter.visibility);
      input.tags = existing.frontMatter.tags;
      if (params.contains("tags") && params["tags"].is_array()) {
        input.tags = params["tags"].get<std::vector<std::string>>();
      }

      const auto rec = documents.update(path, input);
      auditLog.record("update_document", path, true, "updated");
      return textContent("Updated " + rec.path);
    } catch (const vault::DocumentNotFoundError&) {
      auditLog.record("update_document", path, false, "document not found");
      throw mcp_exception(error_code::invalid_params, "document not found: " + path);
    } catch (const vault::PathTraversalError&) {
      auditLog.record("update_document", path, false, "path traversal rejected");
      throw mcp_exception(error_code::invalid_params, "invalid path");
    } catch (const std::exception& e) {
      auditLog.record("update_document", path, false, e.what());
      throw mcp_exception(error_code::internal_error, e.what());
    }
  };
}

// content_base64 is the in-memory path for small files (screenshots,
// snippets). A 120 MiB PDF cannot travel this way — the model would have
// to emit ~160 MiB of JSON, and decoding it would allocate a matching
// std::string. Large files use source_path instead (stdio only). This
// cap is a JSON-size gate, not AttachmentService's own (there isn't one).
constexpr size_t kMaxEncodedAttachmentBytes = 36ull * 1024 * 1024;

std::string decodeContentBase64Param(const json& params) {
  if (!params.contains("content_base64") || !params["content_base64"].is_string()) {
    throw mcp_exception(error_code::invalid_params, "missing 'content_base64'");
  }
  const std::string raw = params["content_base64"].get<std::string>();
  if (raw.size() > kMaxEncodedAttachmentBytes) {
    throw mcp_exception(error_code::invalid_params,
                                "content_base64 is too large; pass source_path for big files");
  }
  const auto decoded = util::decodeBase64(util::stripDataUrlPrefix(raw));
  if (!decoded) {
    throw mcp_exception(error_code::invalid_params, "invalid base64");
  }
  return *decoded;
}

tool_handler makeAttachFileHandler(vault::DocumentService& documents,
                                           vault::AttachmentService& attachments,
                                           index::McpAuditLog& auditLog,
                                           LazyEmbeddingProvider& lazyProvider) {
  return [&documents, &attachments, &auditLog, &lazyProvider](
             const json& params) -> json {
    if (!params.contains("path") || !params["path"].is_string() ||
        params["path"].get<std::string>().empty()) {
      throw mcp_exception(error_code::invalid_params, "missing 'path'");
    }
    const std::string path = params["path"].get<std::string>();
    const bool hasSource = params.contains("source_path") && params["source_path"].is_string() &&
                            !params["source_path"].get<std::string>().empty();
    const bool hasB64 = params.contains("content_base64") && params["content_base64"].is_string();
    if (hasSource && hasB64) {
      throw mcp_exception(error_code::invalid_params,
                                  "pass either source_path or content_base64, not both");
    }
    if (!hasSource && !hasB64) {
      throw mcp_exception(error_code::invalid_params,
                                  "missing 'source_path' or 'content_base64'");
    }

    std::string filename;
    if (params.contains("filename") && params["filename"].is_string()) {
      filename = params["filename"].get<std::string>();
    }
    if (filename.empty() && hasSource) {
      filename = std::filesystem::path(params["source_path"].get<std::string>()).filename().string();
    }
    if (filename.empty()) {
      throw mcp_exception(error_code::invalid_params, "missing 'filename'");
    }
    lazyProvider.ensureLoaded();

    try {
      vault::AttachedFile attached;
      if (hasSource) {
        attached = vault::attachFileAndLinkFromPath(
            documents, attachments, path, filename,
            std::filesystem::path(params["source_path"].get<std::string>()));
      } else {
        const std::string content = decodeContentBase64Param(params);
        attached = vault::attachFileAndLink(documents, attachments, path, filename, content);
      }
      auditLog.record("attach_file", path, true, attached.info.relativePath);
      return textContent("Attached " + attached.info.relativePath + " (" +
                         std::to_string(attached.info.size) + " bytes, " +
                         attached.info.mimeType + ")\nInserted markdown: " +
                         attached.markdownLink);
    } catch (const mcp_exception&) {
      auditLog.record("attach_file", path, false, "invalid or missing content_base64");
      throw;
    } catch (const vault::DocumentNotFoundError&) {
      auditLog.record("attach_file", path, false, "document not found");
      throw mcp_exception(error_code::invalid_params, "document not found: " + path);
    } catch (const vault::AttachmentRejectedError& e) {
      auditLog.record("attach_file", path, false, e.what());
      throw mcp_exception(error_code::invalid_params, e.what());
    } catch (const vault::PathTraversalError&) {
      auditLog.record("attach_file", path, false, "path traversal rejected");
      throw mcp_exception(error_code::invalid_params, "invalid path");
    } catch (const std::filesystem::filesystem_error& e) {
      auditLog.record("attach_file", path, false, e.what());
      throw mcp_exception(error_code::invalid_params, "invalid path");
    } catch (const std::exception& e) {
      auditLog.record("attach_file", path, false, e.what());
      throw mcp_exception(error_code::internal_error, e.what());
    }
  };
}

}  // namespace

void runServer(const std::string& serverName, const std::string& serverVersion,
               index::FtsSearch& search, index::NavQueries& nav,
               index::IndexUpdater& indexUpdater, vault::DocumentService& documents,
               vault::AttachmentService& attachments, index::McpAuditLog& auditLog,
               index::QueryBlocks& queryBlocks, index::CalendarQueries& calendarQueries,
               bool includePrivate, bool writeAccess, const config::AppConfig& cfg) {
  LazyEmbeddingProvider lazyProvider(indexUpdater, cfg);

  server srv(serverName, serverVersion);

  tool searchDocumentsTool =
      tool_builder("search_documents")
          .with_description(
              "Full-text search over the wiki's documents (FTS5, ranked). "
              "Returns matching documents with a highlighted snippet.")
          .with_string_param("query", "Search text", true)
          .with_array_param("tags", "Require all of these tags", "string", false)
          .with_string_param("type", "Filter by document type", false)
          .with_number_param("limit", "Max results (default 20)", false)
          .build();
  srv.register_tool(searchDocumentsTool,
                     makeSearchDocumentsHandler(search, includePrivate));

  tool getDocumentTool =
      tool_builder("get_document")
          .with_description(
              "Fetch one document's full markdown body and metadata, by "
              "its vault-relative path or its id (uuid).")
          .with_string_param("id_or_path", "Document path or id", true)
          .build();
  srv.register_tool(getDocumentTool,
                     makeGetDocumentHandler(documents, indexUpdater, includePrivate));

  tool listTagsTool =
      tool_builder("list_tags")
          .with_description("List every tag in use, with document counts.")
          .build();
  srv.register_tool(listTagsTool, makeListTagsHandler(nav, includePrivate));

  tool listDocumentsTool =
      tool_builder("list_documents")
          .with_description(
              "Browse/list documents (no search text) with optional tag/"
              "type/folder filters and pagination.")
          .with_string_param("tag", "Filter by exact tag", false)
          .with_string_param("type", "Filter by document type", false)
          .with_string_param("folder", "Filter by path prefix, e.g. \"notes/\"", false)
          .with_number_param("limit", "Page size (default 50)", false)
          .with_number_param("offset", "Page offset (default 0)", false)
          .build();
  srv.register_tool(listDocumentsTool,
                     makeListDocumentsHandler(search, includePrivate));

  tool runQueryTool =
      tool_builder("run_query")
          .with_description(
              "Execute a wiki ```query fenced block (the live table the "
              "human sees on a page). Pass the block BODY only — the "
              "key: value lines (type, tag, folder, search, sort, order, "
              "limit, orphans, todos, links), not the surrounding ``` "
              "fences. Same whitelisted DSL as GET /api/query and the "
              "Draft/Chat agent's run_query_block; never raw SQL.")
          .with_string_param("query", "Query-block body, one key: value per line", true)
          .build();
  srv.register_tool(runQueryTool, makeRunQueryHandler(queryBlocks, includePrivate));

  tool getCalendarEventsTool =
      tool_builder("get_calendar_events")
          .with_description(
              "Calendar events (documents with a due date, same engine as "
              "/calendar and GET /api/calendar) in a date range, inclusive. "
              "Recurring series are already expanded into concrete per-day "
              "occurrences — one row per occurrence, not one per document.")
          .with_string_param("start", "Start date, inclusive, YYYY-MM-DD", true)
          .with_string_param("end", "End date, inclusive, YYYY-MM-DD", true)
          .with_string_param("folder", "Restrict to a path prefix, e.g. \"notes/\"", false)
          .with_array_param("tags", "Restrict to documents having all of these tags",
                             "string", false)
          .build();
  srv.register_tool(getCalendarEventsTool,
                     makeGetCalendarEventsHandler(calendarQueries, includePrivate));

  // Absent from tools/list entirely when writeAccess is false — not
  // registered-but-erroring. An MCP client asking "what can you do"
  // never even learns these exist unless the admin opted in.
  if (writeAccess) {
    tool createDocumentTool =
        tool_builder("create_document")
            .with_description(
                "Create a new document in the wiki. Fails if a document "
                "already exists at that path. A path that does not already "
                "end in .md has .md appended.")
            .with_string_param("path", "Vault-relative path, e.g. \"notes/foo.md\" "
                                      "(the .md suffix is optional)", true)
            .with_string_param("title", "Document title", false)
            .with_string_param("body", "Markdown body", false)
            .with_string_param("type", "Document type, e.g. \"note\"", false)
            .with_string_param("visibility", "\"public\" or \"private\" (default private)", false)
            .with_array_param("tags", "Tags for this document", "string", false)
            .with_string_param("due", "ISO8601 date (YYYY-MM-DD) for the calendar, or omit for none",
                                false)
            .with_string_param("recur",
                                "Recurrence rule for the calendar (see docs/calendar.md), or omit "
                                "for a one-off",
                                false)
            .build();
    srv.register_tool(createDocumentTool,
                       makeCreateDocumentHandler(documents, auditLog, lazyProvider));

    tool updateDocumentTool =
        tool_builder("update_document")
            .with_description(
                "Update an existing document. Any field left out keeps its "
                "current value — this is a partial update, not a full "
                "replace.")
            .with_string_param("path", "Vault-relative path of the document to update", true)
            .with_string_param("title", "New title (omit to keep current)", false)
            .with_string_param("body", "New markdown body (omit to keep current)", false)
            .with_string_param("type", "New document type (omit to keep current)", false)
            .with_string_param("visibility",
                                "New \"public\"/\"private\" (omit to keep current)", false)
            .with_array_param("tags", "New tag list (omit to keep current)", "string", false)
            .with_string_param("due",
                                "New ISO8601 due date, or \"\" to clear it (omit to keep current)",
                                false)
            .with_string_param("recur",
                                "New recurrence rule, or \"\" to clear it (omit to keep current)",
                                false)
            .build();
    srv.register_tool(updateDocumentTool,
                       makeUpdateDocumentHandler(documents, auditLog, lazyProvider));

    tool attachFileTool =
        tool_builder("attach_file")
            .with_description(
                "Attach a file to an existing document and append a markdown "
                "link (image embed for png/jpg/gif/webp, a regular link "
                "otherwise). For a large file (a 120 MB PDF, a video) pass "
                "source_path — an absolute path on THIS machine that wiki-mcp "
                "can read; the bytes are copied from disk, never through the "
                "model. content_base64 is only for small files (the file "
                "bytes as standard or URL-safe base64; a data: URL prefix is "
                "stripped). Pass exactly one of source_path or "
                "content_base64. filename defaults to the source basename. "
                "The owning document must already exist.")
            .with_string_param("path", "Vault-relative path of the owning document", true)
            .with_string_param("filename",
                                "Stored filename including extension (defaults to "
                                "source_path's basename)",
                                false)
            .with_string_param("source_path",
                                "Absolute path of a local file to copy into the vault",
                                false)
            .with_string_param("content_base64",
                                "Small file bytes, base64-encoded (not for large PDFs)",
                                false)
            .build();
    srv.register_tool(attachFileTool,
                       makeAttachFileHandler(documents, attachments, auditLog, lazyProvider));
  }

  // CRITICAL: nothing in this process may ever write to stdout except
  // start_stdio()'s own JSON-RPC framing above — any stray std::cout (a
  // debug print, a library that logs there by default, ...) corrupts the
  // pipe and the MCP client sees garbage. All our own diagnostics go to
  // stderr, and start_stdio() blocks until stdin closes.
  srv.start_stdio();
}

}  // namespace wikicore::mcp
