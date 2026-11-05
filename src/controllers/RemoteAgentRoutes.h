#pragma once

#include "auth/McpRemoteConfig.h"
#include "auth/RateLimiter.h"
#include "index/CalendarQueries.h"
#include "index/FtsSearch.h"
#include "index/IndexUpdater.h"
#include "index/McpAuditLog.h"
#include "index/NavQueries.h"
#include "llm/ChatClient.h"
#include "vault/DocumentService.h"

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

// POST /mcp/agent — a remote, bearer-token-gated endpoint that hands a
// short free-text instruction + conversation history to the wiki's OWN
// configured [llm] (RemoteAgentRuntime.h), which can search/browse/read
// documents and the calendar, then decides on its own when to call
// create_document. A deliberately separate trust tier from POST /mcp
// (RemoteMcpRoutes.cpp): there, the caller names the exact tool and
// arguments, so mcp_audit_log fully explains "what happened and why" from
// the request alone; here, the model itself decides what to read and
// write, so a token needs an EXTRA, explicit opt-in
// (McpRemoteConfig::RemoteMcpToken::agentAccess, default off) on top of
// the already-required global writeEnabled — a token scoped to ordinary
// deterministic CRUD does not automatically get this.
//
// Same gating order as POST /mcp (feature enabled -> rate limit -> bearer
// token -> IP allowlist), then: global writeEnabled, then the matched
// token's own agentAccess bit (403 on either — permission checked before
// server state, so a token without agentAccess never learns whether an
// LLM even happens to be configured), then finally whether an LLM is
// actually configured ([llm].provider != "none" — 503 if not, since the
// caller already proved a valid, permitted credential, unlike the
// 404-for-everyone-anonymous used when remote MCP itself is off). Runs
// its own ChatClient instance distinct from the in-app Draft/Chat
// agent's — see RemoteAgentRuntime.h for why sharing one is unsafe.
void registerRemoteAgentRoutes(drogon::HttpAppFramework& app,
                                wikicore::auth::McpRemoteConfig& remoteConfig,
                                wikicore::auth::RateLimiter& rateLimiter,
                                wikicore::llm::ChatClient* chat,
                                wikicore::vault::DocumentService& documents,
                                wikicore::index::FtsSearch& search,
                                wikicore::index::NavQueries& nav,
                                wikicore::index::CalendarQueries& calendar,
                                wikicore::index::IndexUpdater& indexUpdater,
                                wikicore::index::McpAuditLog& auditLog,
                                std::string systemPrompt, bool trustProxyHeaders);

}  // namespace wikicore::controllers
