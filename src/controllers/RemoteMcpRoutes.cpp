#include "controllers/RemoteMcpRoutes.h"

#include "auth/ClientIp.h"
#include "core/wikicore.h"
#include "util/Base64.h"
#include "vault/AttachToDocument.h"
#include "vault/McpUploadStaging.h"

#include <drogon/HttpResponse.h>
#include <drogon/MultiPart.h>

#include <cctype>
#include <filesystem>
#include <string_view>

using namespace drogon;
using namespace wikicore::auth;
using namespace wikicore::index;
using namespace wikicore::vault;

namespace wikicore::controllers {

namespace {

// --- JSON-RPC envelope helpers ---------------------------------------

Json::Value jsonRpcResult(const Json::Value& id, const Json::Value& result) {
  Json::Value body;
  body["jsonrpc"] = "2.0";
  body["id"] = id;
  body["result"] = result;
  return body;
}

Json::Value jsonRpcError(const Json::Value& id, int code, const std::string& message) {
  Json::Value body;
  body["jsonrpc"] = "2.0";
  body["id"] = id;
  Json::Value err;
  err["code"] = code;
  err["message"] = message;
  body["error"] = err;
  return body;
}

// MCP tool-call result shape: {"content": [{"type":"text","text":"..."}],
// "isError": bool} -- identical to what McpServer.cpp's stdio handlers
// return, just built with jsoncpp instead of cpp-mcp's vendored
// nlohmann::ordered_json (see that file's own comment on why the two
// never mix in one translation unit; this is a SEPARATE translation
// unit, so there's no ODR concern, just a different JSON library by
// convention -- every other controller in this project already uses
// jsoncpp, not nlohmann).
Json::Value toolTextResult(const std::string& text, bool isError = false) {
  Json::Value contentItem;
  contentItem["type"] = "text";
  contentItem["text"] = text;
  Json::Value content(Json::arrayValue);
  content.append(contentItem);
  Json::Value result;
  result["content"] = content;
  result["isError"] = isError;
  return result;
}

// --- tool schemas (tools/list) -- same names/descriptions/params as ---
// --- McpServer.cpp's stdio tools, hand-mirrored in jsoncpp -------------

Json::Value stringParam(const std::string& description) {
  Json::Value p;
  p["type"] = "string";
  p["description"] = description;
  return p;
}
Json::Value numberParam(const std::string& description) {
  Json::Value p;
  p["type"] = "number";
  p["description"] = description;
  return p;
}
Json::Value stringArrayParam(const std::string& description) {
  Json::Value p;
  p["type"] = "array";
  p["description"] = description;
  Json::Value items;
  items["type"] = "string";
  p["items"] = items;
  return p;
}

Json::Value toolSchema(const std::string& name, const std::string& description,
                        const Json::Value& properties, const std::vector<std::string>& required) {
  Json::Value schema;
  schema["properties"] = properties;
  schema["type"] = "object";
  if (!required.empty()) {
    Json::Value req(Json::arrayValue);
    for (const auto& r : required) req.append(r);
    schema["required"] = req;
  }
  Json::Value tool;
  tool["name"] = name;
  tool["description"] = description;
  tool["inputSchema"] = schema;
  return tool;
}

Json::Value buildToolsList(bool writeEnabled) {
  Json::Value tools(Json::arrayValue);

  {
    Json::Value props;
    props["query"] = stringParam("Search text");
    props["tags"] = stringArrayParam("Require all of these tags");
    props["type"] = stringParam("Filter by document type");
    props["limit"] = numberParam("Max results (default 20)");
    tools.append(toolSchema(
        "search_documents",
        "Full-text search over the wiki's documents (FTS5, ranked). Returns "
        "matching documents with a highlighted snippet.",
        props, {"query"}));
  }
  {
    Json::Value props;
    props["id_or_path"] = stringParam("Document path or id");
    tools.append(toolSchema(
        "get_document",
        "Fetch one document's full markdown body and metadata, by its "
        "vault-relative path or its id (uuid).",
        props, {"id_or_path"}));
  }
  {
    Json::Value props(Json::objectValue);
    tools.append(toolSchema("list_tags", "List every tag in use, with document counts.",
                             props, {}));
  }
  {
    Json::Value props;
    props["tag"] = stringParam("Filter by exact tag");
    props["type"] = stringParam("Filter by document type");
    props["folder"] = stringParam("Filter by path prefix, e.g. \"notes/\"");
    props["limit"] = numberParam("Page size (default 50)");
    props["offset"] = numberParam("Page offset (default 0)");
    tools.append(toolSchema(
        "list_documents",
        "Browse/list documents (no search text) with optional tag/type/folder "
        "filters and pagination.",
        props, {}));
  }

  // Absent from the list entirely when write access is off -- not
  // present-and-erroring (same rule as the local stdio server's
  // [mcp].write_access; see McpServer.cpp).
  if (writeEnabled) {
    {
      Json::Value props;
      props["path"] = stringParam("Vault-relative path, e.g. \"notes/foo.md\" "
                                 "(the .md suffix is optional)");
      props["title"] = stringParam("Document title");
      props["body"] = stringParam("Markdown body");
      props["type"] = stringParam("Document type, e.g. \"note\"");
      props["visibility"] = stringParam("\"public\" or \"private\" (default private)");
      props["tags"] = stringArrayParam("Tags for this document");
      tools.append(toolSchema(
          "create_document",
          "Create a new document in the wiki. Fails if a document already "
          "exists at that path. A path that does not already end in .md has "
          ".md appended.",
          props, {"path"}));
    }
    {
      Json::Value props;
      props["path"] = stringParam("Vault-relative path of the document to update");
      props["title"] = stringParam("New title (omit to keep current)");
      props["body"] = stringParam("New markdown body (omit to keep current)");
      props["type"] = stringParam("New document type (omit to keep current)");
      props["visibility"] = stringParam("New \"public\"/\"private\" (omit to keep current)");
      props["tags"] = stringArrayParam("New tag list (omit to keep current)");
      tools.append(toolSchema(
          "update_document",
          "Update an existing document. Any field left out keeps its current "
          "value -- this is a partial update, not a full replace.",
          props, {"path"}));
    }
    {
      Json::Value props;
      props["path"] = stringParam("Vault-relative path of the owning document");
      props["filename"] =
          stringParam("Stored filename including extension (defaults to source basename)");
      props["source_path"] = stringParam(
          "Host filesystem path to copy (stdio wiki-mcp only; rejected here)");
      props["content_base64"] = stringParam("Small file bytes, base64-encoded");
      tools.append(toolSchema(
          "attach_file",
          "Attach a file to an existing document and append a markdown link. "
          "On this HTTP transport, pass content_base64 for small files. "
          "For a large file (a 120 MB PDF) call attach_file_begin, then HTTP "
          "PUT the raw bytes to /mcp/uploads/{upload_id} on the same host "
          "(the id is a capability URL — no Bearer token on that PUT). "
          "source_path is rejected here. The owning document must already "
          "exist.",
          props, {"path"}));
    }
    {
      Json::Value props;
      props["path"] = stringParam("Vault-relative path of the owning document");
      props["filename"] = stringParam("Stored filename including extension");
      tools.append(toolSchema(
          "attach_file_begin",
          "Start a large-file upload to an existing document. Returns an "
          "upload_id. Then PUT the raw file bytes (or POST multipart field "
          "'file') to /mcp/uploads/{upload_id} on the same origin as this "
          "MCP endpoint. That request does not need the Bearer token — "
          "knowing the id is enough. The file is attached and linked when "
          "the PUT completes. Prefer this over content_base64 for anything "
          "bigger than a screenshot.",
          props, {"path", "filename"}));
    }
  }

  Json::Value result;
  result["tools"] = tools;
  return result;
}

// --- tool execution -- mirrors McpServer.cpp's handler logic, just ----
// --- against Json::Value/jsoncpp instead of ::mcp::json ---------------

// Remote MCP is always effectively includePrivate=true (scoped by the
// bearer token / IP allowlist instead of the local-stdio-vs-anonymous-
// web-visitor distinction the rest of the app uses) -- there's no
// separate "public remote scope" concept the way [mcp].scope offers for
// stdio; an admin who doesn't want remote callers seeing private
// documents should not enable remote MCP for a vault containing any, the
// same way they wouldn't hand out the bearer token to someone they don't
// trust with full read access.
constexpr bool kIncludePrivate = true;

Json::Value searchResultToJson(const SearchResultItem& item) {
  Json::Value j;
  j["path"] = item.path;
  j["title"] = item.title;
  j["visibility"] = item.visibility;
  j["type"] = item.docType;
  Json::Value tags(Json::arrayValue);
  for (const auto& t : item.tags) tags.append(t);
  j["tags"] = tags;
  j["updated"] = item.updatedAt;
  // Same swap McpServer.cpp does: FTS5 snippet() match markers (control
  // bytes, see FtsSearch.h) become markdown bold for a text-reading
  // client, not raw control bytes and not HTML <mark> (there's no HTML
  // render step on this path to escape-then-substitute through).
  if (item.snippetIsHighlighted) {
    std::string out;
    out.reserve(item.snippet.size());
    for (char c : item.snippet) {
      if (c == FtsSearch::kSnippetMatchStart || c == FtsSearch::kSnippetMatchEnd) {
        out += "**";
      } else {
        out += c;
      }
    }
    j["snippet"] = out;
  } else {
    j["snippet"] = item.snippet;
  }
  return j;
}

Json::Value handleSearchDocuments(FtsSearch& search, const Json::Value& args) {
  if (!args.isMember("query") || !args["query"].isString() || args["query"].asString().empty()) {
    return toolTextResult("missing or empty 'query'", true);
  }
  SearchQuery q;
  q.text = args["query"].asString();
  q.includePrivate = kIncludePrivate;
  q.limit = args.get("limit", 20).asInt();
  if (args.isMember("type") && args["type"].isString()) q.docType = args["type"].asString();
  if (args.isMember("tags") && args["tags"].isArray()) {
    for (const auto& t : args["tags"]) {
      if (t.isString()) q.tags.push_back(t.asString());
    }
  }
  Json::Value arr(Json::arrayValue);
  for (const auto& item : search.search(q)) arr.append(searchResultToJson(item));
  Json::Value out;
  out["results"] = arr;
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "  ";
  return toolTextResult(Json::writeString(writer, arr));
}

Json::Value handleGetDocument(DocumentService& documents, IndexUpdater& indexUpdater,
                               const Json::Value& args) {
  if (!args.isMember("id_or_path") || !args["id_or_path"].isString()) {
    return toolTextResult("missing 'id_or_path'", true);
  }
  const std::string idOrPath = args["id_or_path"].asString();

  auto tryGet = [&](const std::string& path) -> std::optional<DocumentRecord> {
    try {
      return documents.get(path);
    } catch (const DocumentNotFoundError&) {
      return std::nullopt;
    } catch (const PathTraversalError&) {
      return std::nullopt;
    }
  };

  auto record = tryGet(idOrPath);
  if (!record) {
    if (const auto resolvedPath = indexUpdater.findPathByUuid(idOrPath)) {
      record = tryGet(*resolvedPath);
    }
  }
  if (!record || (record->frontMatter.visibility != "public" && !kIncludePrivate)) {
    return toolTextResult("document not found: " + idOrPath, true);
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
  return toolTextResult(out);
}

Json::Value handleListTags(NavQueries& nav) {
  Json::Value arr(Json::arrayValue);
  for (const auto& t : nav.tagCounts(kIncludePrivate)) {
    Json::Value item;
    item["tag"] = t.tag;
    item["count"] = static_cast<Json::Int64>(t.count);
    arr.append(item);
  }
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "  ";
  return toolTextResult(Json::writeString(writer, arr));
}

Json::Value handleListDocuments(FtsSearch& search, const Json::Value& args) {
  SearchQuery q;
  q.includePrivate = kIncludePrivate;
  q.limit = args.get("limit", 50).asInt();
  q.offset = args.get("offset", 0).asInt();
  if (args.isMember("tag") && args["tag"].isString()) q.tag = args["tag"].asString();
  if (args.isMember("type") && args["type"].isString()) q.docType = args["type"].asString();
  if (args.isMember("folder") && args["folder"].isString()) {
    q.folderPrefix = args["folder"].asString();
  }
  Json::Value arr(Json::arrayValue);
  for (const auto& item : search.search(q)) {
    Json::Value j;
    j["path"] = item.path;
    j["title"] = item.title;
    j["visibility"] = item.visibility;
    j["type"] = item.docType;
    Json::Value tags(Json::arrayValue);
    for (const auto& t : item.tags) tags.append(t);
    j["tags"] = tags;
    j["updated"] = item.updatedAt;
    j["excerpt"] = item.snippet;
    arr.append(j);
  }
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "  ";
  return toolTextResult(Json::writeString(writer, arr));
}

// `source` is "remote:" -- prefixed distinctly from the stdio server's
// plain "create_document"/"update_document"/"attach_file" tool_name values in the SAME
// mcp_audit_log table, so an admin reviewing it can tell local-stdio and
// remote-HTTP writes apart at a glance without a schema change (a
// `source` column would need its own migration; this needs none).
Json::Value handleCreateDocument(DocumentService& documents, McpAuditLog& auditLog,
                                  const Json::Value& args) {
  if (!args.isMember("path") || !args["path"].isString() || args["path"].asString().empty()) {
    return toolTextResult("missing 'path'", true);
  }
  const std::string path = args["path"].asString();

  DocumentInput input;
  input.title = args.get("title", "").asString();
  input.body = args.get("body", "").asString();
  input.type = args.get("type", "").asString();
  input.visibility = args.get("visibility", "private").asString();
  if (args.isMember("tags") && args["tags"].isArray()) {
    for (const auto& t : args["tags"]) {
      if (t.isString()) input.tags.push_back(t.asString());
    }
  }

  try {
    const auto rec = documents.create(path, input);
    auditLog.record("remote:create_document", path, true, "created");
    return toolTextResult("Created " + rec.path);
  } catch (const DocumentAlreadyExistsError&) {
    auditLog.record("remote:create_document", path, false,
                     "a document already exists at that path");
    return toolTextResult("a document already exists at that path", true);
  } catch (const PathTraversalError&) {
    auditLog.record("remote:create_document", path, false, "path traversal rejected");
    return toolTextResult("invalid path", true);
  } catch (const std::filesystem::filesystem_error& e) {
    // The audit log keeps the full detail (it's admin-only, DB-stored,
    // recorded for exactly this kind of forensic value) -- but the value
    // handed back to the calling MCP client over the open-internet remote
    // transport must not carry the absolute on-disk vault path embedded in
    // filesystem_error::what(). Same "internal path never reaches the
    // caller" rule as every route in DocumentRoutes.cpp/FolderRoutes.cpp/
    // VersionRoutes.cpp, arguably higher stakes here: this IS the public
    // HTTP surface (docs/architecture.md's "Remote (HTTP) transport").
    auditLog.record("remote:create_document", path, false, e.what());
    return toolTextResult("invalid path", true);
  } catch (const std::exception& e) {
    auditLog.record("remote:create_document", path, false, e.what());
    return toolTextResult(e.what(), true);
  }
}

Json::Value handleUpdateDocument(DocumentService& documents, McpAuditLog& auditLog,
                                  const Json::Value& args) {
  if (!args.isMember("path") || !args["path"].isString() || args["path"].asString().empty()) {
    return toolTextResult("missing 'path'", true);
  }
  const std::string path = args["path"].asString();

  try {
    const DocumentRecord existing = documents.get(path);

    DocumentInput input;
    input.title = args.get("title", existing.frontMatter.title).asString();
    input.body = args.get("body", existing.body).asString();
    input.type = args.get("type", existing.frontMatter.type).asString();
    input.visibility = args.get("visibility", existing.frontMatter.visibility).asString();
    input.tags = existing.frontMatter.tags;
    if (args.isMember("tags") && args["tags"].isArray()) {
      input.tags.clear();
      for (const auto& t : args["tags"]) {
        if (t.isString()) input.tags.push_back(t.asString());
      }
    }

    const auto rec = documents.update(path, input);
    auditLog.record("remote:update_document", path, true, "updated");
    return toolTextResult("Updated " + rec.path);
  } catch (const DocumentNotFoundError&) {
    auditLog.record("remote:update_document", path, false, "document not found");
    return toolTextResult("document not found: " + path, true);
  } catch (const PathTraversalError&) {
    auditLog.record("remote:update_document", path, false, "path traversal rejected");
    return toolTextResult("invalid path", true);
  } catch (const std::filesystem::filesystem_error& e) {
    // See handleCreateDocument's identical catch above for why this can't
    // fall through to the generic e.what() catch below.
    auditLog.record("remote:update_document", path, false, e.what());
    return toolTextResult("invalid path", true);
  } catch (const std::exception& e) {
    auditLog.record("remote:update_document", path, false, e.what());
    return toolTextResult(e.what(), true);
  }
}

// content_base64 is the in-memory path for small files. Large files on
// this HTTP transport cannot use source_path (that would read arbitrary
// files off the wiki host) — call attach_file_begin, then PUT the raw
// bytes to /mcp/uploads/{id}. Stdio wiki-mcp still uses source_path.
constexpr size_t kMaxEncodedAttachmentBytes = 36ull * 1024 * 1024;

Json::Value handleAttachFile(DocumentService& documents, AttachmentService& attachments,
                              McpAuditLog& auditLog, const Json::Value& args) {
  if (!args.isMember("path") || !args["path"].isString() || args["path"].asString().empty()) {
    return toolTextResult("missing 'path'", true);
  }
  const std::string path = args["path"].asString();
  const bool hasSource = args.isMember("source_path") && args["source_path"].isString() &&
                          !args["source_path"].asString().empty();
  const bool hasB64 = args.isMember("content_base64") && args["content_base64"].isString();
  if (hasSource) {
    auditLog.record("remote:attach_file", path, false,
                     "source_path is not available on remote MCP");
    return toolTextResult(
        "source_path is only available on local stdio wiki-mcp; copying a "
        "path on this HTTP server would read arbitrary host files. For a "
        "small file pass content_base64; for a large file call "
        "attach_file_begin, then PUT the bytes to /mcp/uploads/{upload_id}.",
        true);
  }
  if (!hasB64) {
    return toolTextResult("missing 'content_base64'", true);
  }
  std::string filename = args.get("filename", "").asString();
  if (filename.empty()) {
    return toolTextResult("missing 'filename'", true);
  }
  const std::string raw = args["content_base64"].asString();
  if (raw.size() > kMaxEncodedAttachmentBytes) {
    auditLog.record("remote:attach_file", path, false,
                     "content_base64 is too large; use attach_file_begin");
    return toolTextResult(
        "content_base64 is too large for this JSON transport; call "
        "attach_file_begin and PUT the raw bytes to /mcp/uploads/{upload_id}",
        true);
  }
  const auto decoded = wikicore::util::decodeBase64(wikicore::util::stripDataUrlPrefix(raw));
  if (!decoded) {
    auditLog.record("remote:attach_file", path, false, "invalid base64");
    return toolTextResult("invalid base64", true);
  }

  try {
    const auto attached =
        attachFileAndLink(documents, attachments, path, filename, *decoded);
    auditLog.record("remote:attach_file", path, true, attached.info.relativePath);
    return toolTextResult("Attached " + attached.info.relativePath + " (" +
                          std::to_string(attached.info.size) + " bytes, " +
                          attached.info.mimeType + ")\nInserted markdown: " +
                          attached.markdownLink);
  } catch (const DocumentNotFoundError&) {
    auditLog.record("remote:attach_file", path, false, "document not found");
    return toolTextResult("document not found: " + path, true);
  } catch (const AttachmentRejectedError& e) {
    auditLog.record("remote:attach_file", path, false, e.what());
    return toolTextResult(e.what(), true);
  } catch (const PathTraversalError&) {
    auditLog.record("remote:attach_file", path, false, "path traversal rejected");
    return toolTextResult("invalid path", true);
  } catch (const std::filesystem::filesystem_error& e) {
    auditLog.record("remote:attach_file", path, false, e.what());
    return toolTextResult("invalid path", true);
  } catch (const std::exception& e) {
    auditLog.record("remote:attach_file", path, false, e.what());
    return toolTextResult(e.what(), true);
  }
}

Json::Value handleAttachFileBegin(DocumentService& documents, McpUploadStaging& staging,
                                   McpAuditLog& auditLog, const Json::Value& args) {
  if (!args.isMember("path") || !args["path"].isString() || args["path"].asString().empty()) {
    return toolTextResult("missing 'path'", true);
  }
  if (!args.isMember("filename") || !args["filename"].isString() ||
      args["filename"].asString().empty()) {
    return toolTextResult("missing 'filename'", true);
  }
  const std::string path = args["path"].asString();
  const std::string filename = args["filename"].asString();
  try {
    (void)documents.get(path);  // must exist, same as attach_file
    const std::string id = staging.begin(path, filename);
    auditLog.record("remote:attach_file_begin", path, true, id);
    std::string msg;
    msg += "upload_id: ";
    msg += id;
    msg += "\nPUT the raw file bytes to /mcp/uploads/";
    msg += id;
    msg += " (same host and URL prefix as this MCP endpoint; "
           "if MCP is at https://example/wiki/mcp, PUT "
           "https://example/wiki/mcp/uploads/";
    msg += id;
    msg += "). No Authorization header on that PUT — the upload_id is the "
           "capability secret. Multipart POST with field name 'file' also "
           "works. The document is updated with a markdown link when the "
           "upload completes. Ticket expires in 1 hour.";
    return toolTextResult(msg);
  } catch (const DocumentNotFoundError&) {
    auditLog.record("remote:attach_file_begin", path, false, "document not found");
    return toolTextResult("document not found: " + path, true);
  } catch (const PathTraversalError&) {
    auditLog.record("remote:attach_file_begin", path, false, "path traversal rejected");
    return toolTextResult("invalid path", true);
  } catch (const std::filesystem::filesystem_error& e) {
    auditLog.record("remote:attach_file_begin", path, false, e.what());
    return toolTextResult("invalid path", true);
  } catch (const std::exception& e) {
    auditLog.record("remote:attach_file_begin", path, false, e.what());
    return toolTextResult(e.what(), true);
  }
}

}  // namespace

void registerRemoteMcpRoutes(HttpAppFramework& app, McpRemoteConfig& remoteConfig,
                              RateLimiter& rateLimiter, FtsSearch& search, NavQueries& nav,
                              IndexUpdater& indexUpdater, DocumentService& documents,
                              AttachmentService& attachments, McpUploadStaging& mcpUploads,
                              McpAuditLog& auditLog) {
  app.registerHandler(
      "/mcp",
      [&remoteConfig, &rateLimiter, &search, &nav, &indexUpdater, &documents, &attachments,
       &mcpUploads, &auditLog](const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback) {
        // Feature off -> plain 404, indistinguishable from "this route
        // never existed" (see this file's own header comment on why:
        // no hint to an anonymous prober about whether a token would
        // even help).
        const RemoteMcpSettings settings = remoteConfig.get();
        if (!settings.enabled) {
          auto resp = HttpResponse::newHttpResponse();
          resp->setStatusCode(k404NotFound);
          callback(resp);
          return;
        }

        const std::string ip = clientIp(req);

        // Rate limit BEFORE spending any work on token verification --
        // blunts brute-force token guessing the same way /login's own
        // limiter blunts password guessing (AuthRoutes.cpp).
        if (!rateLimiter.allow(ip)) {
          auto resp = HttpResponse::newHttpResponse();
          resp->setStatusCode(k429TooManyRequests);
          callback(resp);
          return;
        }

        // Bearer token -- checked before the IP allowlist so a caller
        // with neither gets the same 401 a caller with a merely-wrong
        // IP but a right token would NOT get (403) — the token is the
        // primary gate, the allowlist is the optional extra layer (see
        // McpRemoteConfig.h).
        const std::string authHeader = req->getHeader("Authorization");
        constexpr std::string_view kBearerPrefix = "Bearer ";
        const bool hasBearer = authHeader.size() > kBearerPrefix.size() &&
                                authHeader.compare(0, kBearerPrefix.size(), kBearerPrefix) == 0;
        const std::string token = hasBearer ? authHeader.substr(kBearerPrefix.size()) : "";
        if (!hasBearer || !remoteConfig.verifyToken(token)) {
          rateLimiter.recordFailure(ip);
          auto resp = HttpResponse::newHttpResponse();
          resp->setStatusCode(k401Unauthorized);
          callback(resp);
          return;
        }
        rateLimiter.recordSuccess(ip);

        if (!remoteConfig.isIpAllowed(ip)) {
          auto resp = HttpResponse::newHttpResponse();
          resp->setStatusCode(k403Forbidden);
          callback(resp);
          return;
        }

        auto json = req->getJsonObject();
        if (!json || !json->isObject() || !json->isMember("method")) {
          callback(HttpResponse::newHttpJsonResponse(
              jsonRpcError(Json::Value(), -32700, "Parse error")));
          return;
        }
        const Json::Value& request = *json;
        const std::string method = request["method"].asString();
        const bool isNotification = !request.isMember("id");
        const Json::Value id = isNotification ? Json::Value() : request["id"];
        const Json::Value params = request.get("params", Json::Value(Json::objectValue));

        if (method == "notifications/initialized") {
          // A notification gets no response body at all (JSON-RPC 2.0) --
          // 202 Accepted, empty, matching the Streamable HTTP transport's
          // documented behavior for a request with nothing to reply to.
          auto resp = HttpResponse::newHttpResponse();
          resp->setStatusCode(k202Accepted);
          callback(resp);
          return;
        }

        if (method == "initialize") {
          Json::Value result;
          result["protocolVersion"] = "2025-03-26";
          Json::Value capabilities;
          capabilities["tools"] = Json::Value(Json::objectValue);
          result["capabilities"] = capabilities;
          Json::Value serverInfo;
          serverInfo["name"] = "personal-wiki";
          serverInfo["version"] = wikicore::versionString();
          result["serverInfo"] = serverInfo;
          callback(HttpResponse::newHttpJsonResponse(jsonRpcResult(id, result)));
          return;
        }

        if (method == "tools/list") {
          callback(HttpResponse::newHttpJsonResponse(
              jsonRpcResult(id, buildToolsList(settings.writeEnabled))));
          return;
        }

        if (method == "tools/call") {
          if (!params.isMember("name") || !params["name"].isString()) {
            callback(HttpResponse::newHttpJsonResponse(
                jsonRpcError(id, -32602, "missing 'name'")));
            return;
          }
          const std::string toolName = params["name"].asString();
          const Json::Value args = params.get("arguments", Json::Value(Json::objectValue));

          Json::Value result;
          if (toolName == "search_documents") {
            result = handleSearchDocuments(search, args);
          } else if (toolName == "get_document") {
            result = handleGetDocument(documents, indexUpdater, args);
          } else if (toolName == "list_tags") {
            result = handleListTags(nav);
          } else if (toolName == "list_documents") {
            result = handleListDocuments(search, args);
          } else if (toolName == "create_document") {
            // Enforced HERE too, not just by omitting it from
            // tools/list -- a client that calls a tool by name it was
            // never advertised must still be refused, the same
            // "listing something as protected proves nothing on its
            // own" discipline as every filter/route check elsewhere in
            // this app.
            result = settings.writeEnabled
                         ? handleCreateDocument(documents, auditLog, args)
                         : toolTextResult("write access is disabled for remote MCP", true);
          } else if (toolName == "update_document") {
            result = settings.writeEnabled
                         ? handleUpdateDocument(documents, auditLog, args)
                         : toolTextResult("write access is disabled for remote MCP", true);
          } else if (toolName == "attach_file") {
            result = settings.writeEnabled
                         ? handleAttachFile(documents, attachments, auditLog, args)
                         : toolTextResult("write access is disabled for remote MCP", true);
          } else if (toolName == "attach_file_begin") {
            result = settings.writeEnabled
                         ? handleAttachFileBegin(documents, mcpUploads, auditLog, args)
                         : toolTextResult("write access is disabled for remote MCP", true);
          } else {
            callback(HttpResponse::newHttpJsonResponse(
                jsonRpcError(id, -32602, "Tool not found: " + toolName)));
            return;
          }
          callback(HttpResponse::newHttpJsonResponse(jsonRpcResult(id, result)));
          return;
        }

        callback(HttpResponse::newHttpJsonResponse(
            jsonRpcError(id, -32601, "Method not found: " + method)));
      },
      {Post});

  // PUT/POST /mcp/uploads/{uuid} — the follow-up to attach_file_begin.
  // Knowing the UUID is the capability (no Bearer). Same enabled / IP
  // allowlist / writeEnabled gates as POST /mcp; the token stays off this
  // path so an agent can `curl -T file.pdf` without pasting it.
  app.registerHandlerViaRegex(
      "^/mcp/uploads/([0-9a-fA-F-]{36})$",
      [&remoteConfig, &rateLimiter, &documents, &attachments, &mcpUploads, &auditLog](
          const HttpRequestPtr& req,
          std::function<void(const HttpResponsePtr&)>&& callback,
          const std::string& uploadId) {
        const RemoteMcpSettings settings = remoteConfig.get();
        if (!settings.enabled) {
          auto resp = HttpResponse::newHttpResponse();
          resp->setStatusCode(k404NotFound);
          callback(resp);
          return;
        }

        const std::string ip = clientIp(req);
        if (!rateLimiter.allow(ip)) {
          auto resp = HttpResponse::newHttpResponse();
          resp->setStatusCode(k429TooManyRequests);
          callback(resp);
          return;
        }

        if (!remoteConfig.isIpAllowed(ip)) {
          auto resp = HttpResponse::newHttpResponse();
          resp->setStatusCode(k403Forbidden);
          callback(resp);
          return;
        }

        if (!settings.writeEnabled) {
          auto resp = HttpResponse::newHttpResponse();
          resp->setStatusCode(k403Forbidden);
          callback(resp);
          return;
        }

        const auto ticket = mcpUploads.get(uploadId);
        if (!ticket) {
          auto resp = HttpResponse::newHttpResponse();
          resp->setStatusCode(k404NotFound);
          callback(resp);
          return;
        }

        std::string bytes;
        std::string filename = ticket->filename;
        std::string ct = req->getHeader("content-type");
        for (char& c : ct) {
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (req->contentType() == CT_MULTIPART_FORM_DATA ||
            ct.find("multipart/form-data") != std::string::npos) {
          MultiPartParser parser;
          if (parser.parse(req) != 0 || parser.getFiles().empty()) {
            Json::Value err;
            err["error"] = "expected exactly one uploaded file";
            auto resp = HttpResponse::newHttpJsonResponse(err);
            resp->setStatusCode(k400BadRequest);
            callback(resp);
            return;
          }
          const auto& file = parser.getFiles()[0];
          bytes.assign(file.fileContent().data(), file.fileContent().size());
          if (!file.getFileName().empty()) filename = file.getFileName();
        } else {
          const auto body = req->body();
          bytes.assign(body.data(), body.size());
        }
        if (bytes.empty()) {
          Json::Value err;
          err["error"] = "empty upload";
          auto resp = HttpResponse::newHttpJsonResponse(err);
          resp->setStatusCode(k400BadRequest);
          callback(resp);
          return;
        }

        try {
          const auto attached = attachFileAndLink(documents, attachments,
                                                   ticket->documentPath, filename, bytes);
          mcpUploads.remove(uploadId);
          auditLog.record("remote:attach_file", ticket->documentPath, true,
                           attached.info.relativePath);
          Json::Value body;
          body["path"] = attached.info.relativePath;
          body["mimeType"] = attached.info.mimeType;
          body["size"] = static_cast<Json::UInt64>(attached.info.size);
          body["markdownLink"] = attached.markdownLink;
          auto resp = HttpResponse::newHttpJsonResponse(body);
          resp->setStatusCode(k201Created);
          callback(resp);
        } catch (const DocumentNotFoundError&) {
          auditLog.record("remote:attach_file", ticket->documentPath, false,
                           "document not found");
          Json::Value err;
          err["error"] = "owning document not found";
          auto resp = HttpResponse::newHttpJsonResponse(err);
          resp->setStatusCode(k404NotFound);
          callback(resp);
        } catch (const AttachmentRejectedError& e) {
          auditLog.record("remote:attach_file", ticket->documentPath, false, e.what());
          Json::Value err;
          err["error"] = e.what();
          auto resp = HttpResponse::newHttpJsonResponse(err);
          resp->setStatusCode(k400BadRequest);
          callback(resp);
        } catch (const PathTraversalError&) {
          auditLog.record("remote:attach_file", ticket->documentPath, false,
                           "path traversal rejected");
          Json::Value err;
          err["error"] = "invalid path";
          auto resp = HttpResponse::newHttpJsonResponse(err);
          resp->setStatusCode(k400BadRequest);
          callback(resp);
        } catch (const std::filesystem::filesystem_error& e) {
          auditLog.record("remote:attach_file", ticket->documentPath, false, e.what());
          Json::Value err;
          err["error"] = "invalid path";
          auto resp = HttpResponse::newHttpJsonResponse(err);
          resp->setStatusCode(k400BadRequest);
          callback(resp);
        } catch (const std::exception& e) {
          auditLog.record("remote:attach_file", ticket->documentPath, false, e.what());
          Json::Value err;
          err["error"] = e.what();
          auto resp = HttpResponse::newHttpJsonResponse(err);
          resp->setStatusCode(k500InternalServerError);
          callback(resp);
        }
      },
      {Put, Post});
}

}  // namespace wikicore::controllers
