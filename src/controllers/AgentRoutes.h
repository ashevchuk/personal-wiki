#pragma once

#include "llm/AgentRuntime.h"

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

// Drafting agent JSON API. The cloud model is called from wiki-server
// as a plain chat-completions client (tools in the JSON body). MCP is
// not involved — Claude/OpenAI never see stdio wiki-mcp or POST /mcp.
//
//   POST   /api/agent/sessions              {instruction, path, title,
//                                           tags, type, body, selection, isNew}
//   GET    /api/agent/sessions/{id}
//   POST   /api/agent/sessions/{id}/messages  same snapshot + instruction
//   POST   /api/agent/sessions/{id}/cancel
//   DELETE /api/agent/sessions/{id}
//
// All five require admin+CSRF (GET is admin-only, no CSRF). Hidden from
// the UI when AgentRuntime::enabled() is false; the handlers still 404
// in that case so a guessed URL is not a working back door.
void registerAgentRoutes(drogon::HttpAppFramework& app, llm::AgentRuntime& agent);

}  // namespace wikicore::controllers
