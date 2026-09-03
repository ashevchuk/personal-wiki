#pragma once

#include <string>
#include <string_view>

namespace wikicore::util {

// Normalizes a configured mount-point prefix ([server].base_path, e.g.
// "wiki", "/wiki", "/wiki/") down to either "" (no prefix — the default,
// current on-root behavior) or a clean "/xxx" form with no trailing
// slash, regardless of what the admin actually typed in config.toml.
std::string normalizeBasePath(std::string_view raw);

// Prepends an already-normalized `basePath` to an absolute in-app path
// (must itself start with '/', e.g. "/login", "/d/foo.md"). Every
// href/action/hx-*/redirect-Location this app emits goes through this —
// see AppConfig::basePath's doc comment for why: it lets a reverse proxy
// mount the whole app under a URL prefix via a PLAIN prefix-stripping
// proxy_pass, with no response-body rewriting (nginx sub_filter) or
// Location-header rewriting (proxy_redirect) needed on the proxy side at
// all, because every URL the app itself hands back to the browser already
// carries the prefix baked in.
std::string withBasePath(const std::string& basePath, std::string_view path);

}  // namespace wikicore::util
