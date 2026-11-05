#pragma once

#include "index/CalendarQueries.h"
#include "index/FtsSearch.h"
#include "index/IndexUpdater.h"
#include "index/McpAuditLog.h"
#include "index/NavQueries.h"
#include "llm/ChatClient.h"
#include "vault/DocumentService.h"

#include <string>
#include <vector>

namespace wikicore::llm {

struct RemoteAgentTurn {
  std::string reply;
  std::vector<std::string> documentsCreated;
};

// Stateless, single-turn tool-calling loop for the remote (bearer-token)
// agent endpoint -- POST /mcp/agent, RemoteAgentRoutes.cpp. Deliberately
// NOT AgentRuntime: that class is a stateful, in-memory, single-flight
// session manager built around browser concepts (editor snapshots, SSE
// generation polling, chat history persisted for the Account page's chat
// list) that don't fit a stateless remote caller, and its "only one
// session running at a time" constraint would make a remote call block on
// or conflict with whatever the admin happens to be doing in Chat/Draft
// right now. This runs its own loop against its OWN ChatClient instance,
// never AgentRuntime's -- CloudChatClient::complete()'s cancel-state
// (activeClient_/stopRequested_) is per-instance, shared mutable state
// unsafe to drive concurrently from two unrelated callers.
//
// Tools: search_documents/list_tags/list_documents/get_document/
// get_calendar_events (read) plus create_document (the only write tool).
// The dividing line that matters here is NOT "read vs write" in general --
// it's drawn at two specific things this class deliberately does NOT
// expose: update_document/attach_file (letting the model silently rewrite
// or overwrite EXISTING content from free text is a materially bigger
// mistake-blast-radius than creating something new, with or without any
// adversary in the picture), and graph link/backlink data on get_document
// (present on AgentRuntime's own version, omitted here to avoid one more
// service dependency for a detail that isn't needed to answer a question
// or avoid a duplicate -- add it if that changes).
class RemoteAgentRuntime {
 public:
  RemoteAgentRuntime(ChatClient& chat, vault::DocumentService& documents,
                      index::FtsSearch& search, index::NavQueries& nav,
                      index::CalendarQueries& calendar, index::IndexUpdater& indexUpdater,
                      index::McpAuditLog& auditLog, std::string systemPrompt);

  // `history` is the caller's own prior user/assistant turns (plain text
  // only -- no system or tool messages; those are internal to this one
  // call and never echoed back). Bounded to a handful of tool-call
  // steps, same shape as AgentRuntime's own loop, and returns immediately
  // after create_document succeeds rather than spending another model
  // round-trip on a confirmation message. On a tool failure (e.g. a path
  // collision, or a get_document miss), the error is fed back to the
  // model and the loop continues, so it can ask a clarifying question or
  // retry with different arguments instead of just failing the whole
  // turn.
  RemoteAgentTurn run(const std::vector<ChatMessage>& history,
                      const std::string& tokenLabel);

 private:
  ChatClient& chat_;
  vault::DocumentService& documents_;
  index::FtsSearch& search_;
  index::NavQueries& nav_;
  index::CalendarQueries& calendar_;
  index::IndexUpdater& indexUpdater_;
  index::McpAuditLog& auditLog_;
  std::string systemPrompt_;
};

}  // namespace wikicore::llm
