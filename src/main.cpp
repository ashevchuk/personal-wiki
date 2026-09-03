// wiki-server — HTTP entrypoint (Drogon).
//
// Milestone 0: bootstrap "hello world" — confirms the CMake/vcpkg toolchain
// produces a working Drogon binary that links against libwikicore. Real
// controllers (auth, documents, search, nav, attachments, admin) land in
// M1–M3; see /home/slayer/.claude/plans/zazzy-twirling-sundae.md.

#include <drogon/drogon.h>

#include "core/wikicore.h"

int main() {
  LOG_INFO << "starting, " << wikicore::versionString();

  drogon::app().registerHandler(
      "/healthz",
      [](const drogon::HttpRequestPtr&,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setBody("ok\n");
        resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        callback(resp);
      },
      {drogon::Get});

  drogon::app().addListener("127.0.0.1", 8080).setThreadNum(2).run();

  return 0;
}
