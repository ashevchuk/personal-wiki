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

}  // namespace

void runServer(const std::string& serverName, const std::string& serverVersion,
               index::FtsSearch& search, index::NavQueries& nav,
               index::IndexUpdater& indexUpdater, vault::DocumentService& documents,
               bool includePrivate) {
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

  // CRITICAL: nothing in this process may ever write to stdout except the
  // library's own JSON-RPC framing — any stray std::cout (a debug print, a
  // library that logs there by default, ...) corrupts the pipe and the
  // MCP client sees garbage. All our own diagnostics go to stderr, and
  // start_stdio() blocks until stdin closes.
  srv.start_stdio();
}

}  // namespace wikicore::mcp
