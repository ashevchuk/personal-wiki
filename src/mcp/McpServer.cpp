#include "mcp/McpServer.h"

// From the vendored hkr04/cpp-mcp library (FetchContent, see
// CMakeLists.txt) — mcp::json is nlohmann::ordered_json, vendored by that
// library under common/json.hpp. Deliberately never mix this file's JSON
// handling with this project's own vcpkg nlohmann_json dependency (used
// elsewhere for e.g. the edit page's doc-data blob) — two different
// vendored copies of the same header in one translation unit is an ODR
// risk not worth taking for zero benefit. Everything in this file goes
// through mcp::json exclusively.
#include <mcp_server.h>
#include <mcp_tool.h>

#include <stdexcept>

using namespace wikicore;

namespace wikicore::mcp {

namespace {

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

::mcp::json searchResultToJson(const index::SearchResultItem& item) {
  return ::mcp::json{
      {"path", item.path},       {"title", item.title},
      {"visibility", item.visibility}, {"type", item.docType},
      {"tags", item.tags},       {"updated", item.updatedAt},
      {"snippet", renderMcpSnippet(item)},
  };
}

::mcp::json textContent(const std::string& text) {
  return ::mcp::json::array({::mcp::json{{"type", "text"}, {"text", text}}});
}

// Applies the fail-safe-private rule independently at the tool layer, on
// top of whatever `includePrivate` already threaded through the query —
// the same category of check that got missed on the HTTP mutating routes
// in M2 (see docs/architecture.md's postmortem) doesn't get skipped here.
bool isVisibleTo(const std::string& visibility, bool includePrivate) {
  return includePrivate || visibility == "public";
}

::mcp::tool_handler makeSearchDocumentsHandler(index::FtsSearch& search,
                                                bool includePrivate) {
  return [&search, includePrivate](const ::mcp::json& params,
                                    const std::string&) -> ::mcp::json {
    if (!params.contains("query") || !params["query"].is_string() ||
        params["query"].get<std::string>().empty()) {
      throw ::mcp::mcp_exception(::mcp::error_code::invalid_params,
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
    ::mcp::json arr = ::mcp::json::array();
    for (const auto& item : results) arr.push_back(searchResultToJson(item));
    return textContent(arr.dump(2));
  };
}

::mcp::tool_handler makeGetDocumentHandler(vault::DocumentService& documents,
                                            index::IndexUpdater& indexUpdater,
                                            bool includePrivate) {
  return [&documents, &indexUpdater, includePrivate](
             const ::mcp::json& params, const std::string&) -> ::mcp::json {
    if (!params.contains("id_or_path") || !params["id_or_path"].is_string()) {
      throw ::mcp::mcp_exception(::mcp::error_code::invalid_params,
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
      throw ::mcp::mcp_exception(::mcp::error_code::invalid_params,
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

::mcp::tool_handler makeListTagsHandler(index::NavQueries& nav, bool includePrivate) {
  return [&nav, includePrivate](const ::mcp::json&, const std::string&) -> ::mcp::json {
    const auto tags = nav.tagCounts(includePrivate);
    ::mcp::json arr = ::mcp::json::array();
    for (const auto& t : tags) {
      arr.push_back(::mcp::json{{"tag", t.tag}, {"count", t.count}});
    }
    return textContent(arr.dump(2));
  };
}

::mcp::tool_handler makeListDocumentsHandler(index::FtsSearch& search,
                                              bool includePrivate) {
  return [&search, includePrivate](const ::mcp::json& params,
                                    const std::string&) -> ::mcp::json {
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
    ::mcp::json arr = ::mcp::json::array();
    for (const auto& item : results) {
      arr.push_back(::mcp::json{
          {"path", item.path},       {"title", item.title},
          {"visibility", item.visibility}, {"type", item.docType},
          {"tags", item.tags},       {"updated", item.updatedAt},
          {"excerpt", item.snippet},  // browse mode: plain excerpt, no markers
      });
    }
    return textContent(arr.dump(2));
  };
}

// A path an LLM client hands us is exactly as untrusted as one from an
// anonymous HTTP request — DocumentService/VaultRepository already
// reject traversal (PathTraversalError), this just makes sure the
// attempt still lands in the audit log rather than only ever showing up
// as a generic error the caller sees but the admin never does.
::mcp::tool_handler makeCreateDocumentHandler(vault::DocumentService& documents,
                                               index::McpAuditLog& auditLog) {
  return [&documents, &auditLog](const ::mcp::json& params,
                                  const std::string&) -> ::mcp::json {
    if (!params.contains("path") || !params["path"].is_string() ||
        params["path"].get<std::string>().empty()) {
      throw ::mcp::mcp_exception(::mcp::error_code::invalid_params, "missing 'path'");
    }
    const std::string path = params["path"].get<std::string>();

    vault::DocumentInput input;
    input.title = params.value("title", std::string());
    input.body = params.value("body", std::string());
    input.type = params.value("type", std::string());
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
      auditLog.record("create_document", path, true, "created");
      return textContent("Created " + rec.path);
    } catch (const vault::DocumentAlreadyExistsError&) {
      auditLog.record("create_document", path, false, "a document already exists at that path");
      throw ::mcp::mcp_exception(::mcp::error_code::invalid_params,
                                  "a document already exists at that path");
    } catch (const vault::PathTraversalError&) {
      auditLog.record("create_document", path, false, "path traversal rejected");
      throw ::mcp::mcp_exception(::mcp::error_code::invalid_params, "invalid path");
    } catch (const std::exception& e) {
      auditLog.record("create_document", path, false, e.what());
      throw ::mcp::mcp_exception(::mcp::error_code::internal_error, e.what());
    }
  };
}

// Merges onto the EXISTING document rather than requiring every field —
// DocumentService::update() itself has no concept of a partial update
// (it always writes a complete DocumentInput, same as the HTTP PUT
// route), so an LLM caller that only means to change the body shouldn't
// have to first fetch and echo back the title/tags/type/visibility it
// isn't touching.
::mcp::tool_handler makeUpdateDocumentHandler(vault::DocumentService& documents,
                                               index::McpAuditLog& auditLog) {
  return [&documents, &auditLog](const ::mcp::json& params,
                                  const std::string&) -> ::mcp::json {
    if (!params.contains("path") || !params["path"].is_string() ||
        params["path"].get<std::string>().empty()) {
      throw ::mcp::mcp_exception(::mcp::error_code::invalid_params, "missing 'path'");
    }
    const std::string path = params["path"].get<std::string>();

    try {
      const vault::DocumentRecord existing = documents.get(path);

      vault::DocumentInput input;
      input.title = params.value("title", existing.frontMatter.title);
      input.body = params.value("body", existing.body);
      input.type = params.value("type", existing.frontMatter.type);
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
      throw ::mcp::mcp_exception(::mcp::error_code::invalid_params, "document not found: " + path);
    } catch (const vault::PathTraversalError&) {
      auditLog.record("update_document", path, false, "path traversal rejected");
      throw ::mcp::mcp_exception(::mcp::error_code::invalid_params, "invalid path");
    } catch (const std::exception& e) {
      auditLog.record("update_document", path, false, e.what());
      throw ::mcp::mcp_exception(::mcp::error_code::internal_error, e.what());
    }
  };
}

}  // namespace

void runServer(const std::string& serverName, const std::string& serverVersion,
               index::FtsSearch& search, index::NavQueries& nav,
               index::IndexUpdater& indexUpdater, vault::DocumentService& documents,
               index::McpAuditLog& auditLog, bool includePrivate, bool writeAccess) {
  ::mcp::server::configuration conf;
  conf.name = serverName;
  conf.version = serverVersion;

  ::mcp::server srv(conf);
  srv.set_server_info(serverName, serverVersion);
  srv.set_capabilities(::mcp::json{{"tools", ::mcp::json::object()}});

  ::mcp::tool searchDocumentsTool =
      ::mcp::tool_builder("search_documents")
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

  ::mcp::tool getDocumentTool =
      ::mcp::tool_builder("get_document")
          .with_description(
              "Fetch one document's full markdown body and metadata, by "
              "its vault-relative path or its id (uuid).")
          .with_string_param("id_or_path", "Document path or id", true)
          .build();
  srv.register_tool(getDocumentTool,
                     makeGetDocumentHandler(documents, indexUpdater, includePrivate));

  ::mcp::tool listTagsTool =
      ::mcp::tool_builder("list_tags")
          .with_description("List every tag in use, with document counts.")
          .build();
  srv.register_tool(listTagsTool, makeListTagsHandler(nav, includePrivate));

  ::mcp::tool listDocumentsTool =
      ::mcp::tool_builder("list_documents")
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

  // Absent from tools/list entirely when writeAccess is false — not
  // registered-but-erroring. An MCP client asking "what can you do"
  // never even learns these exist unless the admin opted in.
  if (writeAccess) {
    ::mcp::tool createDocumentTool =
        ::mcp::tool_builder("create_document")
            .with_description(
                "Create a new document in the wiki. Fails if a document "
                "already exists at that path.")
            .with_string_param("path", "Vault-relative path, e.g. \"notes/foo.md\"", true)
            .with_string_param("title", "Document title", false)
            .with_string_param("body", "Markdown body", false)
            .with_string_param("type", "Document type, e.g. \"note\"", false)
            .with_string_param("visibility", "\"public\" or \"private\" (default private)", false)
            .with_array_param("tags", "Tags for this document", "string", false)
            .build();
    srv.register_tool(createDocumentTool, makeCreateDocumentHandler(documents, auditLog));

    ::mcp::tool updateDocumentTool =
        ::mcp::tool_builder("update_document")
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
            .build();
    srv.register_tool(updateDocumentTool, makeUpdateDocumentHandler(documents, auditLog));
  }

  // CRITICAL: nothing in this process may ever write to stdout except the
  // library's own JSON-RPC framing — any stray std::cout (a debug print, a
  // library that logs there by default, ...) corrupts the pipe and the
  // MCP client sees garbage. All our own diagnostics go to stderr, and
  // start_stdio() blocks until stdin closes.
  srv.start_stdio();
}

}  // namespace wikicore::mcp
