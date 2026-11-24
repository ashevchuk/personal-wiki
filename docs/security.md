# Security

How `wiki-server` and `wiki-mcp` protect private content, which risks are
accepted on purpose, and which tests check each property. User-facing
behavior lives in [`behavior.md`](behavior.md), MCP specifics in
[`mcp.md`](mcp.md), deployment hardening in
[`sbc-deployment.md`](sbc-deployment.md).

## Threat model

One admin account, no roles. The things worth protecting are private
documents and their attachments, the admin session, remote MCP bearer
tokens, and the index database (it holds the admin password hash and the
MCP audit log).

| Actor | Can do | Trust |
|---|---|---|
| Anonymous visitor | Read public documents, their attachments, search and nav over public content | Untrusted |
| Admin (browser session) | Everything | Trusted |
| Remote MCP client (bearer token) | Read tools; write tools only when remote write access is on | Partially trusted: one token per client, separately revocable |
| Remote agent (`POST /mcp/agent`, token with `agent_access`) | An LLM that reads documents and creates new ones from free text | Partially trusted: the input text may come from anywhere |
| Local process spawning `wiki-mcp` | Whatever `[mcp].scope` and `[mcp].write_access` allow | Trusted: spawning the process is the trust boundary |
| Anything writing into the vault directory (editor, `git pull`, sync tool) | Arbitrary document content, picked up by `VaultWatcher` | Trusted as the admin's own files, but the content is not assumed to be safe HTML |

Out of scope: multiple users, a compromised host, a malicious admin.

Document content therefore reaches the browser from several channels, not
only from the admin typing it. Rendering treats every document body,
title, tag and file name as untrusted, whatever channel it came from.

## Defenses

### Authentication and sessions

- Passwords: argon2id, 12 characters minimum, 1024 bytes maximum (checked
  before hashing, so an oversized password costs nothing).
- Session tokens: 256-bit random, stored as SHA-256, compared in constant
  time. Login always issues a fresh token; a cookie set before login is
  never adopted (session fixation). A password change drops every other
  session.
- Session cookie: `HttpOnly`, `SameSite=Lax`, `Path=/`, 14 days.
  `Secure` when the request arrived over TLS, or over a trusted proxy that
  sent `X-Forwarded-Proto: https`.
- Login rate limiting per client address (IPv6 bucketed by /64):
  exponential backoff from 1 s up to 5 minutes, no hard lockout.

### CSRF

- Synchronizer token stored server-side with the session, delivered to the
  page through a second, non-`HttpOnly` cookie, and accepted only in the
  `X-CSRF-Token` header. A query-string or form-field token is rejected.
- A cross-site form cannot set a custom header, so form-encoded and
  `text/plain` POSTs fail whatever the body looks like.
- `AuthFilter` and `CsrfFilter` never reject a request with no session on
  their own. Every mutating handler calls `requireAdminApi(req)` first.
  A route listing both filters is not proof that it is protected; the
  handler is.

### Visibility

- `visibility` defaults to `private` when missing or malformed.
- A private document, or anything derived from it (attachments, search
  hits, tag counts, graph nodes and edges, backlinks, query results),
  returns `404` to an anonymous caller, never `403`. Existence is not
  revealed.
- Every MCP tool re-checks the resolved document's own visibility against
  `[mcp].scope`.

### Paths and files

- `PathGuard` is the only code path that resolves a vault path:
  lexical check, then canonicalization, then a vault-root prefix check.
  Symlink escapes are rejected.
- A path with any dot-prefixed segment (`.trash/`, `.git/`, the index
  database) is refused for every externally supplied document path: HTTP,
  remote MCP, stdio MCP and agent tools.
- Static files are served only from `static/`, never the working
  directory, so `config.toml`, the database and logs next to the binary are
  not reachable over HTTP.
- `filesystem_error` is caught before the generic handler in every
  mutating route, so an absolute vault path never appears in an error
  response.

### Rendering untrusted content

- md4c runs with `MD_FLAG_NOHTMLBLOCKS | MD_FLAG_NOHTMLSPANS`: raw HTML in
  a document body is escaped, never passed through. This is the primary
  defense; there is no separate sanitizer pass.
- `<a href>` keeps only `http`, `https`, `mailto`, `tel`, relative URLs and
  fragments. Other schemes (`javascript:`, `data:`, `vbscript:`, with any
  case, leading control bytes, embedded whitespace or character
  references) lose the `href` and keep the link text.
- YouTube and circuit embeds, and search snippets, use marker-then-
  substitute: inert markup goes through md4c or the escaper first, and only
  the exact tag produced is swapped for real HTML afterwards. YouTube IDs
  are validated at both ends.
- The JSON API returns titles, tags, types and file names as literal text.
  Escaping them is the frontend's job: any such value that reaches an
  `innerHTML` sink must go through `WikiCommon.escapeHtml` or be assigned
  with `textContent` instead.

### Uploads

- Any extension can be uploaded. The file name keeps only
  `[A-Za-z0-9._-]` and well-formed non-ASCII letters; every other
  character becomes `_`. Names are de-duplicated on collision.
- Only a configured list of passive types is rendered inline (images, PDF,
  audio, video, plain text, markdown, CSV, JSON by default,
  `[attachments].inline_safe_extensions`). Everything else, `.html`,
  `.svg`, `.xml` and `.js` included, is served with
  `Content-Disposition: attachment`.
- Every response carries `X-Content-Type-Options: nosniff`, so a file
  served as `image/png` or `text/plain` is never sniffed into HTML.
- A `.md` file inside a `.assets/` folder is an attachment, not a
  document: it is not indexed and not readable through document routes.

### Response headers

Set on every response (`main.cpp`, `registerPreSendingAdvice`):

| Header | Value |
|---|---|
| `Content-Security-Policy` | `default-src 'self'; script-src 'self' 'unsafe-inline'; worker-src 'self'; style-src 'self' 'unsafe-inline'; font-src 'self'; img-src 'self' https: data:; frame-src 'self' https://www.youtube.com; object-src 'none'; base-uri 'self'; form-action 'self'; frame-ancestors 'none'` |
| `X-Content-Type-Options` | `nosniff` |
| `X-Frame-Options` | `DENY` |
| `Referrer-Policy` | `strict-origin-when-cross-origin` |
| `Strict-Transport-Security` | only on TLS connections in standalone TLS mode |

Responses under `/js/circuitjs1/` use `frame-ancestors 'self'` and
`X-Frame-Options: SAMEORIGIN` instead, because the document view embeds
that app in a same-origin iframe. No CORS headers are sent at all.

### Remote MCP and the agent

- `/mcp`, `/mcp/agent` and `/mcp/uploads/{id}` return `404` while remote
  MCP is disabled.
- Bearer tokens: SHA-256 stored, shown once, labeled, revocable
  individually. The IP allowlist is checked before the token, so a caller
  outside the allowlist learns nothing about token validity.
- Remote write access is a separate toggle from `[mcp].write_access`.
  Every write is recorded in `mcp_audit_log` with the token's label.
- The remote agent can read and create documents. It cannot call
  `update_document` or `attach_file`, so free text cannot rewrite existing
  content.
- Upload tickets from `attach_file_begin` are single-use capability URLs.

### Resource limits

- Request bodies are capped at 100 MiB.
- Rate limiter maps are pruned above 10 000 tracked addresses.
- Remote MCP has its own rate limiter.

### Client IP

`auth::clientIp()` trusts `X-Real-IP`, then the last `X-Forwarded-For`
entry, only when `[tls].enabled` is false. That is the nginx-fronted
shape, where the proxy overwrites or appends those headers. In standalone
TLS mode the peer address is used and the headers are ignored. Any new
per-IP control must go through this function.

## Why `'unsafe-inline'` is in `script-src`

`static/shell.html` carries two inline scripts that must run before any
other resource is requested:

1. Base-path detection. It sets `<base>` from the current URL (or from
   `window.__WIKI_KNOWN_BASE_PATH__`, injected by `PageRoutes.cpp`), so
   every relative `href`/`src` resolves under a reverse-proxy subpath.
   It has to run before the parser reaches any `<link>` or `<script src>`.
2. Theme bootstrap. It creates the theme `<link>` and applies the saved
   theme synchronously, so a full page reload never paints or fetches the
   wrong theme first.

`PageRoutes.cpp` also injects `window.__WIKI_THEMES__` and optional config
values as inline script into the same page. A nonce would have to be
generated per response and threaded through the cached shell
(`g_shellHtml`); a hash would have to be recomputed on every edit of those
scripts and of the injected values. `'unsafe-inline'` avoids that
coupling.

`style-src 'unsafe-inline'` is required by the vendored Toast UI Editor,
which renders markup with `style` attributes, and by mermaid, which
inserts `<style>` elements into its diagrams.

Consequence: CSP does not stop injected inline script or `javascript:`
navigation. It still blocks remote scripts, plugins, foreign framing,
`<base>` hijacking and foreign form targets. Protection against XSS rests
on the rendering rules above (no raw HTML, scheme allowlist,
marker-then-substitute) and on escaping at every frontend `innerHTML`
sink.

## Accepted risks

- **No CSP backstop for inline script.** See above.
- **The remote agent may create `public` documents.** The model chooses
  `visibility` (default `private`). Text injected into its input can make
  it publish content it read from private documents.
- **`img-src https:`.** A document can embed any external image, which
  tells that host the viewer's IP and can carry data in the URL. Markdown
  links to external images are a normal use case.
- **`<img src>` schemes are not filtered.** The renderer leaves `<img src>`
  alone because it carries the internal `youtube-embed:` marker. Browsers
  never execute a script URL as an image source, and CSP `img-src` refuses
  it as well.
- **The circuitjs1 header relaxation is a prefix match on the raw request
  path.** A dot-segment such as `/js/circuitjs1/../common.js` carries
  `frame-ancestors 'self'` to another static file. Only this app's own
  origin can frame it.
- **Rate limiter check-then-act race.** `allow()` and `recordFailure()` are
  two locked calls; a concurrent burst of wrong passwords can get a few
  extra attempts in before the block applies.
- **Bodies up to 100 MiB are received before authentication.**
- **Proxy headers are trusted whenever `[tls].enabled` is false.** A
  directly exposed non-TLS listener (for example a published Docker port
  without a proxy) lets a client choose its own address for rate limiting
  and the remote MCP allowlist. Such a deployment needs `[tls]` or a
  reverse proxy.
- **Frontend escaping is not tested in a browser.** The server-side suite
  checks what the API returns; it does not execute the SPA's `innerHTML`
  sinks.

## Verification

| Suite | Run | Covers |
|---|---|---|
| `unit_tests` (Catch2) | `ctest --test-dir build -R unit_tests` | `PathGuard` traversal and symlink payloads, front-matter fail-safe defaults, link-scheme filtering, snippet markers, password hashing, rate limiter, cert reload validation |
| `security_e2e` | `ctest --test-dir build -R security_e2e` | Real server over HTTP, see below |
| `tls_e2e` | `ctest --test-dir build -R tls_e2e` | Standalone TLS termination, HSTS, spoofed `X-Real-IP` ignored for the remote MCP limiter |
| `stress_concurrency`, `stress_resources` | `ctest --test-dir build -L stress` | Concurrent check-then-act paths, two SQLite connections, upload caps, search load vs `/healthz`, long paths |
| Fuzzers | `tests/fuzz/CMakeLists.txt`, Clang + `WIKI_ENABLE_FUZZING` | `PathGuard::resolve()` and `parseFrontMatter()` under ASan/UBSan |

`tests/integration/security_e2e.py` checks:

- Authentication: every mutating and admin route rejects an anonymous
  caller; disabled remote MCP endpoints return `404`; session fixation;
  password policy and length limits; other sessions dropped on password
  change; login rate limiting by client IP; cookie `HttpOnly`,
  `SameSite=Lax` and `Secure`.
- CSRF: missing header, token in the query string, wrong or empty token,
  token only in the cookie, another session's token, form-encoded and
  `text/plain` bodies.
- Visibility: private content absent for anonymous callers across
  documents, raw view, search, nav tree, tags, queries, graph, calendar,
  attachments and trash.
- Rendering: a document full of XSS payloads (raw HTML, script URL
  variants, attribute breakouts, wiki-link, YouTube and circuit
  injections) renders with no executable element, event handler,
  disallowed iframe or script URL, both in `renderedHtml` and in the trash
  excerpt; metadata round-trips as literal JSON text; the raw view is
  `text/plain`.
- Uploads: active types (`.svg`, `.html`, `.htm`, `.xhtml`, `.xml`, `.js`,
  no extension, upper case, double extension) are forced downloads; inline
  types are never served as HTML, SVG or XML; file names cannot break
  `Content-Disposition`.
- Paths: traversal in URLs and payloads, with encoded, double-encoded,
  backslash, `..;`, NUL and absolute shapes across document, asset,
  attachment and static routes; dot-directory paths; files next to the
  binary (`config.toml`, database, log) never served.
- Headers: CSP directives, `nosniff`, `X-Frame-Options`, `Referrer-Policy`
  on pages, API, static files, assets and `404`s; the circuitjs1 exception.
- Request shaping: method-override headers and `_method` ignored, `TRACE`
  does not echo cookies, no CORS grant for a foreign `Origin`, `Host` and
  `X-Forwarded-Host` not reflected, no off-site redirects, no header
  injection through CRLF in a path, client-supplied `id`/`created`/
  `updated` ignored on create, no absolute sandbox path in any response.
- Remote MCP: token lifecycle, independent revocation, write and agent
  toggles, dot-path refusal, host-file `source_path` refusal, single-use
  upload tickets.

## External scanners

Autonomous scanners such as Strix send real exploit traffic and create,
modify and delete data. Point them only at a throwaway instance: a fresh
vault, a test admin account, remote MCP enabled with a dedicated token,
never the production host or a vault with real content. For white-box
mode, give the scanner a clean `git clone`, not a working copy that may
contain `config.toml`, `.notes/` or a vault. Turn every confirmed finding
into a check in `security_e2e.py` or a unit test before fixing it.
