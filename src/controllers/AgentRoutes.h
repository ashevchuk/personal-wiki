#pragma once

#include "llm/AgentRuntime.h"

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

// Drafting agent JSON API. The cloud model is called from wiki-server
// as a plain chat-completions client (tools in the JSON body). MCP is
// not involved — Claude/OpenAI never see stdio wiki-mcp or POST /mcp.
//
//   GET    /api/agent/sessions              {sessions:[{id,title,createdAt,updatedAt}]}
//   POST   /api/agent/sessions              {instruction, kind, view?, ...}
//   GET    /api/agent/sessions/{id}
//   GET    /api/agent/sessions/{id}/stream  text/event-stream (admin, no CSRF)
//   POST   /api/agent/sessions/{id}/messages
//   POST   /api/agent/sessions/{id}/cancel
//   POST   /api/agent/sessions/{id}/title   {title}
//   DELETE /api/agent/sessions/{id}
//
// Mutating routes require admin+CSRF; GETs are admin-only, no CSRF. Hidden
// from the UI when AgentRuntime::enabled() is false; the handlers still 404
// in that case so a guessed URL is not a working back door.
void registerAgentRoutes(drogon::HttpAppFramework& app, llm::AgentRuntime& agent);

}  // namespace wikicore::controllers
