#pragma once

#include "vault/VaultRepository.h"

#include <drogon/HttpAppFramework.h>

namespace wikicore::controllers {

// Registers GET /d/{path...}. Milestone 1 scope: read + render only (one
// manually-seeded document, per the plan's M1 acceptance criteria) — full
// CRUD (DocumentService, atomic save, attachments) lands Milestone 2.
void registerDocumentRoutes(drogon::HttpAppFramework& app,
                             wikicore::vault::VaultRepository& vault);

}  // namespace wikicore::controllers
