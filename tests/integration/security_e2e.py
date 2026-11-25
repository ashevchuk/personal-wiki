#!/usr/bin/env python3
"""
End-to-end security/correctness checks against a real wiki-server process:
auth, CSRF, path traversal, session fixation, visibility gating (search/
nav/attachments), rate limiting, admin password change (wrong-current-
password rejection, other-session invalidation), VaultWatcher pickup
of external filesystem changes, and response hardening (security headers,
cookie attributes, active content in rendered HTML, upload serving, CSRF
variants, encoded traversal, method override, CORS). docs/security.md
maps each check to the property it protects.

This exists specifically because unit tests can't catch HTTP-layer wiring
bugs — see the M2 postmortem in docs/architecture.md: an unauthenticated
POST /api/documents once returned 201 and nothing in ctest would have
caught it. This script is that missing net, wired into `ctest` (see
tests/CMakeLists.txt) so it actually runs on every build rather than
living as a one-off shell session.

The backend is a pure JSON API now (see docs/architecture.md's frontend
section — the old server-rendered HTML pages are gone, replaced by a
static SPA shell + client-side JS). This means /login, /d/{path...},
/edit/{path...}, /search, /folder[/...] all return the SAME static shell
regardless of the request — real access-control assertions here target
the JSON endpoints (/api/session, /api/login, /api/documents/{path...},
/api/search, ...) instead, which is where the actual authorization now
lives.

Usage: security_e2e.py <path-to-wiki-server-binary>
"""
import base64
import hashlib
import html.parser
import http.client
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.parse

PORT = 8199
HOST = "127.0.0.1"

FAILURES = []


def check(desc, cond, detail=""):
    if cond:
        print(f"OK   {desc}")
    else:
        print(f"FAIL {desc}" + (f" ({detail})" if detail else ""))
        FAILURES.append(desc)


class Client:
    """Minimal HTTP client (stdlib only) that tracks cookies across requests,
    the way a browser session would."""

    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.cookies = {}

    def _cookie_header(self):
        return "; ".join(f"{k}={v}" for k, v in self.cookies.items())

    def _capture_cookies(self, resp):
        for header, value in resp.getheaders():
            if header.lower() == "set-cookie":
                kv = value.split(";", 1)[0]
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    self.cookies[k] = v

    def request(self, method, path, body=None, headers=None, json_body=None):
        conn = http.client.HTTPConnection(self.host, self.port, timeout=5)
        h = dict(headers or {})
        if self.cookies:
            h["Cookie"] = self._cookie_header()
        data = body
        if json_body is not None:
            data = json.dumps(json_body).encode()
            h["Content-Type"] = "application/json"
        try:
            conn.request(method, path, body=data, headers=h)
            resp = conn.getresponse()
            content = resp.read()
            self._capture_cookies(resp)
            return resp.status, dict(resp.getheaders()), content
        finally:
            conn.close()

    def get(self, path, headers=None):
        return self.request("GET", path, headers=headers)

    def get_json(self, path, headers=None):
        status, hdrs, body = self.get(path, headers=headers)
        return status, hdrs, (json.loads(body) if body else None)

    def post_form(self, path, fields, headers=None):
        body = urllib.parse.urlencode(fields).encode()
        h = dict(headers or {})
        h["Content-Type"] = "application/x-www-form-urlencoded"
        return self.request("POST", path, body=body, headers=h)

    def post_json(self, path, obj, headers=None):
        return self.request("POST", path, json_body=obj, headers=headers)

    def put_json(self, path, obj, headers=None):
        return self.request("PUT", path, json_body=obj, headers=headers)

    def delete(self, path, headers=None):
        return self.request("DELETE", path, headers=headers)

    def upload(self, path, filename, content, headers=None):
        boundary = "----wikiE2EBoundary"
        body = (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="file"; filename="{filename}"\r\n'
            f"Content-Type: application/octet-stream\r\n\r\n"
        ).encode() + content + f"\r\n--{boundary}--\r\n".encode()
        h = dict(headers or {})
        h["Content-Type"] = f"multipart/form-data; boundary={boundary}"
        return self.request("POST", path, body=body, headers=h)


def wait_for_healthz(timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            c = Client(HOST, PORT)
            status, _, _ = c.get("/healthz")
            if status == 200:
                return True
        except (ConnectionRefusedError, OSError):
            pass
        time.sleep(0.2)
    return False


def main():
    if len(sys.argv) < 2:
        print("usage: security_e2e.py <path-to-wiki-server-binary>", file=sys.stderr)
        return 2
    server_bin = os.path.abspath(sys.argv[1])

    sandbox = tempfile.mkdtemp(prefix="wiki-security-e2e-")
    vault = os.path.join(sandbox, "vault")
    os.makedirs(vault, exist_ok=True)

    with open(os.path.join(sandbox, "config.toml"), "w") as f:
        f.write(f"""
[server]
listen_addr = "{HOST}"
port = {PORT}
threads = 2
[vault]
path = "{vault}"
[index]
db_path = "{sandbox}/index.db"
[mcp]
scope = "admin"
[log]
level = "warn"
""")

    # The shell routes (/, /login, /search, /d/{...}, /edit/{...},
    # /folder[/...]) serve static/shell.html off disk relative to CWD
    # (see PageRoutes.cpp) — same as setDocumentRoot("static") always
    # needed static/ present relative to CWD for CSS/JS, this sandbox
    # needs its own copy of it too, mirroring what a real deployment's
    # WorkingDirectory=/opt/wiki (containing both bin/ and static/) does.
    project_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    shutil.copytree(os.path.join(project_root, "static"), os.path.join(sandbox, "static"))

    admin_proc = subprocess.run(
        [server_bin, "--create-admin"], cwd=sandbox,
        input="admin\nSuperSecret123\nSuperSecret123\n",
        text=True, capture_output=True,
    )
    if admin_proc.returncode != 0:
        print("--create-admin failed:", admin_proc.stdout, admin_proc.stderr)
        shutil.rmtree(sandbox, ignore_errors=True)
        return 1

    server = subprocess.Popen(
        [server_bin], cwd=sandbox,
        stdout=open(os.path.join(sandbox, "server.log"), "w"),
        stderr=subprocess.STDOUT,
    )

    try:
        if not wait_for_healthz():
            print("server never became healthy")
            FAILURES.append("server startup")
            return 1

        run_checks(sandbox, vault, server_bin)

    finally:
        server.send_signal(signal.SIGTERM)
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
        if FAILURES:
            print("\n--- server.log tail (failures occurred) ---")
            with open(os.path.join(sandbox, "server.log")) as f:
                print("".join(f.readlines()[-30:]))
        shutil.rmtree(sandbox, ignore_errors=True)

    print(f"\n{'=' * 60}")
    if FAILURES:
        print(f"{len(FAILURES)} FAILURE(S):")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print("All security/E2E checks passed.")
    return 0


ACTIVE_TAGS = {"script", "object", "embed", "applet", "base", "meta", "link", "style",
               "form", "frame", "frameset", "svg", "math", "template", "noscript"}
URL_ATTRS = {"href", "src", "action", "formaction", "xlink:href", "background",
             "poster", "data", "srcset"}
ALLOWED_IFRAME_SRC_PREFIXES = ("https://www.youtube.com/embed/", "js/circuitjs1/")


class ActiveContentFinder(html.parser.HTMLParser):
    """Collects every element or attribute in server-rendered HTML that a
    browser could execute or that changes how the page loads. The parser
    decodes entities in attribute values, so `java&#9;script:` arrives here
    in the form a browser would act on."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.problems = []

    def handle_starttag(self, tag, attrs):
        if tag in ACTIVE_TAGS:
            self.problems.append(f"<{tag}>")
        for name, value in attrs:
            value = value or ""
            if name.startswith("on"):
                self.problems.append(f"<{tag} {name}=...>")
            if name == "srcdoc":
                self.problems.append(f"<{tag} srcdoc>")
            if name in URL_ATTRS:
                url = re.sub(r"[\x00-\x20]", "", value).lower()
                # Browsers never execute a script URL as an <img> source,
                # and the renderer leaves <img src> alone on purpose
                # (MarkdownRenderer.cpp, stripUnsafeLinkHrefs).
                image_source = tag == "img" and name in ("src", "srcset")
                if url.startswith(("javascript:", "vbscript:")) and not image_source:
                    self.problems.append(f"<{tag} {name}={value!r}>")
                if url.startswith("data:") and not (tag == "img" and name == "src"):
                    self.problems.append(f"<{tag} {name}={value!r}>")
        if tag == "iframe":
            src = dict(attrs).get("src") or ""
            if not src.startswith(ALLOWED_IFRAME_SRC_PREFIXES):
                self.problems.append(f"<iframe src={src!r}>")


def active_content(rendered_html):
    finder = ActiveContentFinder()
    finder.feed(rendered_html)
    finder.close()
    return finder.problems


def lower_headers(hdrs):
    return {k.lower(): v for k, v in hdrs.items()}


def csp_directives(csp):
    directives = {}
    for part in csp.split(";"):
        tokens = part.split()
        if tokens:
            directives[tokens[0]] = set(tokens[1:])
    return directives


XSS_BODY = """\
<script>alert('raw-script')</script>
<img src=x onerror=alert('raw-img')>
<svg onload=alert('raw-svg')>
<iframe src="https://attacker.example/"></iframe>
[plain](javascript:alert('plain'))
[mixed-case](JaVaScRiPt:alert('case'))
[entity-tab](java&#9;script:alert('tab'))
[entity-colon](javascript&colon;alert('colon'))
[leading-space](<  javascript:alert('space')>)
[vb](vbscript:msgbox('vb'))
[data](data:text/html;base64,PHNjcmlwdD5hbGVydCgxKTwvc2NyaXB0Pg==)
<javascript:alert('autolink')>
![img-js](javascript:alert('img'))
[ref-link][evil]
[attr-break](https://example.com/"onmouseover="alert('attr'))
![img-attr-break](https://example.com/x.png"onerror="alert('img-attr'))
[[<img src=x onerror=alert('wikilink')>]]
[[xss/target]]
[[xss/target|<script>alert('wikilink-label')</script>]]
![youtube](https://www.youtube.com/watch?v="><script>alert('yt')</script>)
![youtube](https://youtu.be/aaaaaaaaaaa"onload="alert('yt-attr'))

```circuit
"></iframe><script>alert('circuit')</script><iframe src="
```

[evil]: javascript:alert('ref')
"""

XSS_TITLE = "<img src=x onerror=alert('title')>"


def run_hardening_checks(sandbox, vault, anon, admin, csrf, attach_path):
    """Response hardening and parser-edge checks: security headers, cookie
    attributes, active content in server-rendered HTML, upload serving,
    CSRF variants, encoded traversal, file exposure, method override, CORS,
    Host-header reflection. Every response body seen here is also checked
    for the sandbox's absolute path."""
    leaks = []

    def fetch(client, method, path, **kwargs):
        status, hdrs, body = client.request(method, path, **kwargs)
        if sandbox.encode() in body:
            leaks.append(f"{method} {path}")
        return status, lower_headers(hdrs), body

    csrf_header = {"X-CSRF-Token": csrf}

    # --- Security headers on every kind of response ---------------------
    header_probes = [
        ("shell page", anon, "/"),
        ("JSON API", anon, "/api/session"),
        ("JSON 404", anon, "/api/documents/does-not-exist.md"),
        ("static file", anon, "/js/common.js"),
        ("static 404", anon, "/no-such-file.css"),
        ("asset", admin, f"/assets/{attach_path}"),
    ]
    for label, client, path in header_probes:
        status, hdrs, _ = fetch(client, "GET", path)
        csp = csp_directives(hdrs.get("content-security-policy", ""))
        check(f"headers ({label}): CSP default-src/object-src/base-uri/frame-ancestors locked",
              csp.get("default-src") == {"'self'"} and csp.get("object-src") == {"'none'"}
              and csp.get("base-uri") == {"'self'"} and csp.get("frame-ancestors") == {"'none'"},
              f"status={status} csp={csp}")
        check(f"headers ({label}): script-src loads no remote origin",
              csp.get("script-src") == {"'self'", "'unsafe-inline'"},
              f"script-src={csp.get('script-src')}")
        check(f"headers ({label}): nosniff, X-Frame-Options DENY, Referrer-Policy",
              hdrs.get("x-content-type-options") == "nosniff"
              and hdrs.get("x-frame-options") == "DENY"
              and hdrs.get("referrer-policy") == "strict-origin-when-cross-origin",
              f"got {hdrs.get('x-content-type-options')!r} {hdrs.get('x-frame-options')!r} "
              f"{hdrs.get('referrer-policy')!r}")
        check(f"headers ({label}): no CORS grant without an Origin",
              "access-control-allow-origin" not in hdrs, f"got {hdrs.get('access-control-allow-origin')!r}")
    status, hdrs, _ = fetch(anon, "GET", "/js/circuitjs1/circuitjs.html")
    csp = csp_directives(hdrs.get("content-security-policy", ""))
    check("headers (circuitjs1 embed): framable by this origin only",
          status == 200 and csp.get("frame-ancestors") == {"'self'"}
          and hdrs.get("x-frame-options") == "SAMEORIGIN",
          f"status={status} frame-ancestors={csp.get('frame-ancestors')} "
          f"xfo={hdrs.get('x-frame-options')!r}")
    status, hdrs, _ = fetch(anon, "GET", "/js/circuitjs1/../common.js")
    # The circuitjs1 relaxation is a prefix match on the raw request path, so
    # a dot-segment can carry it to another static file; it still only ever
    # allows this app's own origin to frame the response.
    check("headers: a dot-segment out of circuitjs1/ is still unframable by a foreign origin",
          csp_directives(hdrs.get("content-security-policy", "")).get("frame-ancestors")
          in ({"'none'"}, {"'self'"}) and hdrs.get("x-frame-options") in ("DENY", "SAMEORIGIN"),
          f"status={status} csp={hdrs.get('content-security-policy')!r}")

    # --- Cookie attributes ----------------------------------------------
    conn = http.client.HTTPConnection(HOST, PORT, timeout=5)
    try:
        conn.request("POST", "/api/login",
                     body=json.dumps({"username": "admin", "password": "SuperSecret123"}),
                     headers={"Content-Type": "application/json"})
        resp = conn.getresponse()
        resp.read()
        set_cookies = [v for k, v in resp.getheaders() if k.lower() == "set-cookie"]
    finally:
        conn.close()
    by_name = {c.split("=", 1)[0]: c.lower() for c in set_cookies}
    session_cookie = by_name.get("wiki_session", "")
    csrf_cookie = by_name.get("wiki_csrf_token", "")
    check("session cookie is HttpOnly and SameSite=Lax",
          "; httponly" in session_cookie and "samesite=lax" in session_cookie,
          f"got {session_cookie!r}")
    check("csrf cookie is SameSite=Lax and readable by the page (not HttpOnly)",
          csrf_cookie and "samesite=lax" in csrf_cookie and "httponly" not in csrf_cookie,
          f"got {csrf_cookie!r}")

    # --- Active content in server-rendered HTML -------------------------
    status, _, _ = fetch(admin, "POST", "/api/documents", headers=csrf_header, json_body={
        "path": "xss/target.md", "title": XSS_TITLE, "visibility": "public",
        "type": "<script>alert('type')</script>", "tags": ["<script>alert('tag')</script>"],
        "body": "target"})
    check("create XSS-titled link target -> 201", status == 201, f"got {status}")
    status, _, _ = fetch(admin, "POST", "/api/documents", headers=csrf_header, json_body={
        "path": "xss/payloads.md", "title": XSS_TITLE, "visibility": "public",
        "body": XSS_BODY})
    check("create XSS payload doc -> 201", status == 201, f"got {status}")
    status, hdrs, raw = fetch(anon, "GET", "/api/documents/xss/payloads.md")
    doc = json.loads(raw) if status == 200 else {}
    check("XSS doc JSON is served as application/json",
          hdrs.get("content-type", "").startswith("application/json"),
          f"got {hdrs.get('content-type')!r}")
    problems = active_content(doc.get("renderedHtml", ""))
    check("renderedHtml contains no executable markup, handlers or script URLs",
          status == 200 and not problems, f"status={status} problems={problems}")
    check("metadata round-trips as literal text in JSON (escaping is the client's job)",
          doc.get("title") == XSS_TITLE, f"got {doc.get('title')!r}")
    status, _, raw = fetch(anon, "GET", "/api/documents/xss/target.md")
    target = json.loads(raw) if status == 200 else {}
    check("type and tags round-trip as literal text in JSON",
          target.get("type") == "<script>alert('type')</script>"
          and target.get("tags") == ["<script>alert('tag')</script>"],
          f"type={target.get('type')!r} tags={target.get('tags')!r}")

    status, hdrs, raw = fetch(anon, "GET", "/api/documents/xss/payloads.md/raw")
    check("raw document is served as text/plain, never as HTML",
          status == 200 and hdrs.get("content-type", "").startswith("text/plain")
          and hdrs.get("x-content-type-options") == "nosniff",
          f"status={status} type={hdrs.get('content-type')!r}")

    status, hdrs, raw = fetch(anon, "GET", "/d/%3Cscript%3Ealert(1)%3C/script%3E")
    check("shell page does not reflect a script tag from the URL",
          status == 200 and b"<script>alert(1)" not in raw, f"status={status}")
    status, hdrs, raw = fetch(anon, "GET", "/api/documents/%3Cscript%3Ealert(1)%3C%2Fscript%3E.md")
    check("404 for a script-shaped path is not HTML, and carries nosniff",
          status == 404 and "html" not in hdrs.get("content-type", "")
          and hdrs.get("x-content-type-options") == "nosniff",
          f"status={status} type={hdrs.get('content-type')!r}")

    status, _, _ = fetch(admin, "DELETE", "/api/documents/xss/payloads.md", headers=csrf_header)
    check("trash the XSS payload doc -> 200", status == 200, f"got {status}")
    status, _, raw = fetch(admin, "GET", "/api/admin/trash")
    items = json.loads(raw).get("items", []) if status == 200 else []
    excerpt = next((i.get("excerptHtml", "") for i in items if i.get("path") == "xss/payloads.md"), None)
    check("trash excerptHtml contains no executable markup",
          excerpt is not None and not active_content(excerpt),
          f"excerpt={excerpt!r} problems={active_content(excerpt or '')}")

    # --- Upload serving: active types are forced downloads --------------
    active_uploads = [
        ("evil.svg", b'<svg xmlns="http://www.w3.org/2000/svg" onload="alert(1)"><script>alert(2)</script></svg>'),
        ("evil.html", b"<script>alert(1)</script>"),
        ("evil.htm", b"<script>alert(1)</script>"),
        ("evil.xhtml", b'<html xmlns="http://www.w3.org/1999/xhtml"><script>alert(1)</script></html>'),
        ("evil.xml", b'<x:script xmlns:x="http://www.w3.org/1999/xhtml">alert(1)</x:script>'),
        ("EVIL.SVG", b"<svg onload=alert(1)></svg>"),
        ("evil.png.svg", b"<svg onload=alert(1)></svg>"),
        ("evil.js", b"alert(1)"),
        ("noextension", b"<script>alert(1)</script>"),
    ]
    for filename, content in active_uploads:
        status, _, raw = admin.upload("/api/attachments/notes/private.md", filename, content,
                                      headers=csrf_header)
        if status != 201:
            check(f"upload {filename} -> 201", False, f"got {status}")
            continue
        stored = json.loads(raw)["path"]
        status, hdrs, _ = fetch(admin, "GET", "/assets/" + urllib.parse.quote(stored))
        check(f"{filename} is served as a forced download with nosniff",
              status == 200 and hdrs.get("content-disposition", "").startswith("attachment")
              and hdrs.get("x-content-type-options") == "nosniff",
              f"status={status} disposition={hdrs.get('content-disposition')!r}")
    inline_text_uploads = [("probe.txt", b"<script>alert(1)</script>"),
                           ("probe.md", b"<script>alert(1)</script>"),
                           ("probe.json", b'{"a":"<script>alert(1)</script>"}'),
                           ("polyglot.png", b"<html><script>alert(1)</script></html>")]
    for filename, content in inline_text_uploads:
        status, _, raw = admin.upload("/api/attachments/notes/private.md", filename, content,
                                      headers=csrf_header)
        stored = json.loads(raw)["path"] if status == 201 else ""
        status, hdrs, _ = fetch(admin, "GET", "/assets/" + urllib.parse.quote(stored))
        content_type = hdrs.get("content-type", "")
        check(f"inline {filename} is never served as HTML/SVG/XML, and carries nosniff",
              status == 200 and "html" not in content_type and "svg" not in content_type
              and "xml" not in content_type and hdrs.get("x-content-type-options") == "nosniff",
              f"status={status} type={content_type!r}")
    status, _, raw = admin.upload("/api/attachments/notes/private.md", 'q"uo;te\\d.html',
                                  b"x", headers=csrf_header)
    stored = json.loads(raw)["path"] if status == 201 else ""
    check("quote/semicolon/backslash are stripped from a stored filename",
          status == 201 and not re.search(r'["\\;]', stored.rsplit("/", 1)[-1]),
          f"status={status} stored={stored!r}")
    status, hdrs, _ = fetch(admin, "GET", "/assets/" + urllib.parse.quote(stored))
    check("Content-Disposition stays one well-formed attachment value",
          re.fullmatch(r"attachment; filename=\"[^\"\\]*\"; filename\*=UTF-8''[A-Za-z0-9%._~-]*",
                       hdrs.get("content-disposition", "")) is not None,
          f"got {hdrs.get('content-disposition')!r}")

    # --- CSRF variants ---------------------------------------------------
    def csrf_probe(label, path, **kwargs):
        status, _, _ = fetch(admin, "POST", "/api/documents", **kwargs)
        exists = os.path.exists(os.path.join(vault, path))
        check(f"CSRF: {label} -> 403, nothing written",
              status == 403 and not exists, f"status={status} written={exists}")

    form_body = urllib.parse.urlencode({"path": "csrf/form.md", "title": "x", "body": "y"}).encode()
    csrf_probe("form-encoded POST without token", "csrf/form.md", body=form_body,
               headers={"Content-Type": "application/x-www-form-urlencoded"})
    plain_body = json.dumps({"path": "csrf/plain.md", "title": "x", "body": "y"}).encode()
    csrf_probe("text/plain JSON body without token", "csrf/plain.md", body=plain_body,
               headers={"Content-Type": "text/plain"})
    csrf_probe("wrong token value", "csrf/wrong.md", headers={"X-CSRF-Token": "0" * len(csrf)},
               json_body={"path": "csrf/wrong.md", "title": "x", "body": "y"})
    csrf_probe("empty token header", "csrf/empty.md", headers={"X-CSRF-Token": ""},
               json_body={"path": "csrf/empty.md", "title": "x", "body": "y"})
    csrf_probe("token sent only as a cookie", "csrf/cookie.md",
               json_body={"path": "csrf/cookie.md", "title": "x", "body": "y"})
    other = Client(HOST, PORT)
    status, _, _ = other.post_json("/api/login", {"username": "admin", "password": "SuperSecret123"})
    other_csrf = other.cookies.get("wiki_csrf_token", "")
    check("second session login for the token-binding check -> 200",
          status == 200 and other_csrf and other_csrf != csrf, f"got {status}")
    csrf_probe("another session's token", "csrf/foreign.md",
               headers={"X-CSRF-Token": other_csrf},
               json_body={"path": "csrf/foreign.md", "title": "x", "body": "y"})

    # --- Mass assignment: server-owned front-matter fields --------------
    with open(os.path.join(vault, "notes", "private.md")) as f:
        private_id = re.search(r"^id:\s*(\S+)", f.read(), re.M).group(1)
    status, _, _ = fetch(admin, "POST", "/api/documents", headers=csrf_header, json_body={
        "path": "mass/assign.md", "title": "x", "body": "y", "visibility": "public",
        "id": private_id, "created": "2000-01-01T00:00:00Z", "updated": "2000-01-01T00:00:00Z"})
    with open(os.path.join(vault, "mass", "assign.md")) as f:
        written = f.read()
    check("create ignores a client-supplied id (no uuid collision with another doc)",
          status == 201 and private_id not in written, f"status={status}")
    check("create ignores client-supplied created/updated timestamps",
          "2000-01-01" not in written, f"front matter: {written[:300]!r}")

    # --- Encoded path traversal over HTTP --------------------------------
    traversal_targets = {"config.toml": b"[vault]", "etc/passwd": b"root:"}
    traversal_shapes = [
        "../{t}", "../../../../../../{t}", "%2e%2e/{t}", "%2e%2e%2f{t}", "..%2f{t}",
        "%252e%252e%252f{t}", "..%5c{t}", "....//{t}", "..;/{t}", "%2e%2e%5c{t}",
        "notes/%2e%2e/%2e%2e/{t}", "notes/..%00/../{t}", "/{t}", "%2f{t}",
    ]
    traversal_prefixes = ["/api/documents/", "/assets/", "/api/attachments/", "/", "/js/"]
    leaked = []
    for prefix in traversal_prefixes:
        for shape in traversal_shapes:
            for target, marker in traversal_targets.items():
                path = prefix + shape.format(t=target)
                for suffix in ("", "/raw") if prefix == "/api/documents/" else ("",):
                    try:
                        status, _, body = fetch(admin, "GET", path + suffix)
                    except http.client.HTTPException as e:
                        leaked.append(f"{path}{suffix}: client error {e!r}")
                        continue
                    if marker in body:
                        leaked.append(f"{path}{suffix} -> {status}")
    check("no encoded traversal shape reads config.toml or /etc/passwd (admin session)",
          not leaked, f"leaked={leaked}")
    status, _, _ = fetch(anon, "GET", "/healthz")
    check("server still healthy after the traversal sweep", status == 200, f"got {status}")

    # --- Files that must never be served ---------------------------------
    for path in ("/config.toml", "/index.db", "/index.db-wal", "/server.log",
                 "/static/shell.html", "/static/../config.toml", "/.git/HEAD",
                 "/CMakeLists.txt", "/vault/notes/private.md", "/.uploads-tmp/",
                 "/.trash/", "/.index.db"):
        status, _, body = fetch(admin, "GET", path)
        exposed = status == 200 and (b"[vault]" in body or b"SQLite format" in body
                                     or b"systemd private content" in body)
        check(f"{path} is not served from outside static/", not exposed, f"got {status}")

    # --- Method override, TRACE, CORS -----------------------------------
    status, _, _ = fetch(admin, "POST", "/api/documents", headers=csrf_header, json_body={
        "path": "override/probe.md", "title": "x", "body": "y", "visibility": "public"})
    check("create method-override probe -> 201", status == 201, f"got {status}")
    for method, path, extra in (
            ("POST", "/api/documents/override/probe.md", {"X-HTTP-Method-Override": "DELETE"}),
            ("POST", "/api/documents/override/probe.md", {"X-HTTP-Method": "DELETE"}),
            ("POST", "/api/documents/override/probe.md", {"X-Method-Override": "DELETE"}),
            ("POST", "/api/documents/override/probe.md?_method=DELETE", {}),
            ("GET", "/api/documents/override/probe.md?_method=DELETE", {})):
        fetch(admin, method, path, headers={**csrf_header, **extra})
    status, _, _ = fetch(anon, "GET", "/api/documents/override/probe.md")
    check("method-override headers and _method do not turn a request into DELETE",
          status == 200, f"got {status}")
    status, _, body = fetch(admin, "TRACE", "/api/session")
    check("TRACE does not echo the session cookie back",
          admin.cookies.get("wiki_session", "?").encode() not in body, f"got {status}")

    evil_origin = {"Origin": "https://attacker.example"}
    for method, path in (("GET", "/api/session"), ("GET", "/api/documents/notes/private.md"),
                         ("OPTIONS", "/api/documents")):
        headers = dict(evil_origin)
        if method == "OPTIONS":
            headers.update({"Access-Control-Request-Method": "POST",
                            "Access-Control-Request-Headers": "x-csrf-token, content-type"})
        status, hdrs, _ = fetch(admin, method, path, headers=headers)
        check(f"CORS: {method} {path} from a foreign Origin gets no allow-origin/credentials",
              "access-control-allow-origin" not in hdrs
              and "access-control-allow-credentials" not in hdrs,
              f"status={status} acao={hdrs.get('access-control-allow-origin')!r}")

    # --- Host header, open redirect, response splitting -----------------
    for path in ("/", "/login", "/d/notes/public.md", "/api/session"):
        status, hdrs, body = fetch(anon, "GET", path, headers={
            "Host": "attacker.example", "X-Forwarded-Host": "attacker.example"})
        check(f"Host/X-Forwarded-Host are not reflected ({path})",
              b"attacker.example" not in body and "attacker.example" not in hdrs.get("location", ""),
              f"status={status}")
    for path in ("//attacker.example/", "/%2f%2fattacker.example", "/\\attacker.example",
                 "/login?next=https://attacker.example", "/d/x?returnTo=//attacker.example"):
        status, hdrs, _ = fetch(anon, "GET", path)
        location = hdrs.get("location", "")
        check(f"no off-site redirect for {path}",
              "attacker.example" not in location, f"status={status} location={location!r}")
    status, hdrs, _ = fetch(anon, "GET", "/api/documents/a%0d%0aX-Injected:%201.md")
    check("CRLF in a path does not inject a response header",
          "x-injected" not in hdrs, f"status={status} headers={sorted(hdrs)}")

    check("no response in the hardening checks disclosed the sandbox's absolute path",
          not leaks, f"leaks={leaks}")


MODERN_MCP_VERSION = "2026-07-28"
MODERN_MCP_META = {
    "io.modelcontextprotocol/protocolVersion": MODERN_MCP_VERSION,
    "io.modelcontextprotocol/clientInfo": {"name": "security-e2e", "version": "1"},
    "io.modelcontextprotocol/clientCapabilities": {},
}


def run_modern_mcp_checks(anon, mcp_auth, mcp_bin, sandbox, expected_version):
    """MCP 2026-07-28 (stateless, per-request _meta) served alongside the
    2025-03-26 initialize flow on both transports."""

    def modern_post(method, params=None, rid=1, meta=MODERN_MCP_META, headers=None,
                    omit_headers=(), with_id=True):
        p = dict(params or {})
        if meta is not None:
            p["_meta"] = meta
        h = {"MCP-Protocol-Version": MODERN_MCP_VERSION, "Mcp-Method": method}
        if method == "tools/call" and "name" in p:
            h["Mcp-Name"] = p["name"]
        h.update(mcp_auth)
        h.update(headers or {})
        for k in omit_headers:
            h.pop(k, None)
        msg = {"jsonrpc": "2.0", "method": method, "params": p}
        if with_id:
            msg["id"] = rid
        status, _, body = anon.post_json("/mcp", msg, headers=h)
        try:
            return status, (json.loads(body) if body else {}), body
        except ValueError:
            return status, {}, body

    def error_code(rpc):
        return (rpc.get("error") or {}).get("code")

    status, rpc, body = modern_post("server/discover")
    result = rpc.get("result") or {}
    server_info = (result.get("_meta") or {}).get("io.modelcontextprotocol/serverInfo") or {}
    check("remote modern server/discover -> complete DiscoverResult",
          status == 200 and result.get("resultType") == "complete"
          and result.get("supportedVersions") == [MODERN_MCP_VERSION]
          and "tools" in (result.get("capabilities") or {})
          and isinstance(result.get("ttlMs"), int) and result["ttlMs"] >= 0
          and result.get("cacheScope") == "public"
          and server_info == {"name": "personal-wiki", "version": expected_version},
          f"status={status} body={body[:400]!r}")

    status, rpc, body = modern_post("tools/list")
    result = rpc.get("result") or {}
    tool_names = [t.get("name") for t in result.get("tools") or []]
    check("remote modern tools/list -> resultType + caching hints",
          status == 200 and result.get("resultType") == "complete"
          and "get_document" in tool_names
          and isinstance(result.get("ttlMs"), int) and result.get("cacheScope") == "public",
          f"status={status} body={body[:300]!r}")

    status, rpc, body = modern_post("tools/call", {"name": "list_tags", "arguments": {}})
    result = rpc.get("result") or {}
    check("remote modern tools/call -> resultType complete, serverInfo in _meta",
          status == 200 and result.get("resultType") == "complete"
          and result.get("isError") is False and isinstance(result.get("content"), list)
          and "io.modelcontextprotocol/serverInfo" in (result.get("_meta") or {}),
          f"status={status} body={body[:300]!r}")

    sentinel = "=?base64?" + base64.b64encode(b"list_tags").decode() + "?="
    status, rpc, body = modern_post("tools/call", {"name": "list_tags", "arguments": {}},
                                    headers={"Mcp-Name": sentinel})
    check("remote modern tools/call accepts a base64-sentinel Mcp-Name",
          status == 200 and (rpc.get("result") or {}).get("resultType") == "complete",
          f"status={status} body={body[:300]!r}")

    for desc, kwargs in (
        ("missing MCP-Protocol-Version", {"omit_headers": ("MCP-Protocol-Version",)}),
        ("missing Mcp-Method", {"omit_headers": ("Mcp-Method",)}),
        ("missing Mcp-Name on tools/call", {"omit_headers": ("Mcp-Name",)}),
        ("Mcp-Name differs from params.name", {"headers": {"Mcp-Name": "get_document"}}),
        ("Mcp-Method differs from method", {"headers": {"Mcp-Method": "tools/list"}}),
    ):
        status, rpc, body = modern_post("tools/call", {"name": "list_tags", "arguments": {}},
                                        **kwargs)
        check(f"remote modern request with {desc} -> 400 HeaderMismatch (-32020)",
              status == 400 and error_code(rpc) == -32020, f"status={status} body={body[:300]!r}")

    unsupported_meta = dict(MODERN_MCP_META)
    unsupported_meta["io.modelcontextprotocol/protocolVersion"] = "2099-01-01"
    status, rpc, body = modern_post("tools/list", meta=unsupported_meta,
                                    headers={"MCP-Protocol-Version": "2099-01-01"})
    data = (rpc.get("error") or {}).get("data") or {}
    check("remote modern request with an unsupported version -> 400 -32022 + supported list",
          status == 400 and error_code(rpc) == -32022
          and data.get("supported") == [MODERN_MCP_VERSION] and data.get("requested") == "2099-01-01",
          f"status={status} body={body[:300]!r}")

    no_caps = {k: v for k, v in MODERN_MCP_META.items()
               if k != "io.modelcontextprotocol/clientCapabilities"}
    status, rpc, body = modern_post("tools/list", meta=no_caps)
    check("remote modern request without clientCapabilities -> 400 -32602",
          status == 400 and error_code(rpc) == -32602, f"status={status} body={body[:300]!r}")

    status, rpc, body = modern_post("tools/list", meta=None)
    check("remote request claiming 2026-07-28 by header only (no _meta) -> 400 -32602, "
          "not served as legacy",
          status == 400 and error_code(rpc) == -32602, f"status={status} body={body[:300]!r}")

    status, rpc, body = modern_post("ping")
    check("remote modern unknown/removed method (ping) -> 404 -32601",
          status == 404 and error_code(rpc) == -32601, f"status={status} body={body[:300]!r}")

    status, _, body = modern_post("notifications/cancelled", with_id=False)
    check("remote modern notification -> 202, empty body",
          status == 202 and not body, f"status={status} body={body[:200]!r}")

    status, _, body = modern_post("tools/list", headers={"Origin": "https://evil.example"})
    check("remote modern request with a foreign Origin -> 403",
          status == 403, f"status={status} body={body[:200]!r}")
    status, rpc, body = modern_post(
        "tools/list", headers={"Origin": f"http://{anon.host}:{anon.port}"})
    check("remote modern request with a same-host Origin -> 200",
          status == 200 and (rpc.get("result") or {}).get("resultType") == "complete",
          f"status={status} body={body[:200]!r}")

    for verb in ("GET", "DELETE"):
        status, _, _ = anon.request(verb, "/mcp", headers=mcp_auth)
        check(f"{verb} /mcp -> 405", status == 405, f"got {status}")

    status, _, body = anon.post_json(
        "/mcp", {"jsonrpc": "2.0", "id": 7, "method": "tools/list"},
        headers={**mcp_auth, "MCP-Protocol-Version": "2025-06-18"})
    legacy = json.loads(body).get("result") or {} if body else {}
    check("remote legacy request with a 2025-06-18 header and no _meta stays legacy",
          status == 200 and "tools" in legacy and "resultType" not in legacy,
          f"status={status} body={body[:200]!r}")

    def line(rid, method, params=None, meta=MODERN_MCP_META):
        p = dict(params or {})
        if meta is not None:
            p["_meta"] = meta
        msg = {"jsonrpc": "2.0", "method": method, "params": p}
        if rid is not None:
            msg["id"] = rid
        return json.dumps(msg)

    stdio_input = "\n".join([
        line(1, "server/discover"),
        line(2, "tools/list"),
        line(3, "tools/call", {"name": "list_tags", "arguments": {}}),
        line(4, "tools/list", meta=unsupported_meta),
        line(5, "tools/list", meta=no_caps),
        line(6, "ping"),
        line(None, "notifications/cancelled"),
        line(7, "initialize", meta=None),
        line(8, "tools/list", meta=None),
    ]) + "\n"
    stdio = subprocess.run([mcp_bin], cwd=sandbox, capture_output=True, text=True, timeout=20,
                           input=stdio_input)
    responses = {}
    for out_line in stdio.stdout.splitlines():
        msg = json.loads(out_line)
        responses[msg.get("id")] = msg
    detail = f"stdout={stdio.stdout[:600]!r} stderr={stdio.stderr[-300:]!r}"

    discover = responses.get(1, {}).get("result") or {}
    check("stdio modern server/discover -> complete DiscoverResult",
          discover.get("resultType") == "complete"
          and discover.get("supportedVersions") == [MODERN_MCP_VERSION]
          and discover.get("cacheScope") == "public" and isinstance(discover.get("ttlMs"), int)
          and ((discover.get("_meta") or {}).get("io.modelcontextprotocol/serverInfo") or {})
          .get("version") == expected_version,
          detail)
    tools_list = responses.get(2, {}).get("result") or {}
    check("stdio modern tools/list -> resultType + caching hints",
          tools_list.get("resultType") == "complete" and tools_list.get("tools")
          and tools_list.get("cacheScope") == "public", detail)
    called = responses.get(3, {}).get("result") or {}
    check("stdio modern tools/call -> resultType complete",
          called.get("resultType") == "complete" and called.get("isError") is False, detail)
    err4 = responses.get(4, {}).get("error") or {}
    check("stdio modern unsupported version -> -32022 + supported list",
          err4.get("code") == -32022
          and (err4.get("data") or {}).get("supported") == [MODERN_MCP_VERSION], detail)
    check("stdio modern request without clientCapabilities -> -32602",
          (responses.get(5, {}).get("error") or {}).get("code") == -32602, detail)
    check("stdio modern ping (removed in 2026-07-28) -> -32601",
          (responses.get(6, {}).get("error") or {}).get("code") == -32601, detail)
    check("stdio modern notification gets no response",
          None not in responses and len(responses) == 8, detail)
    init = responses.get(7, {}).get("result") or {}
    legacy_list = responses.get(8, {}).get("result") or {}
    check("stdio legacy initialize + tools/list still work in the same process",
          init.get("protocolVersion") == "2025-03-26" and "tools" in legacy_list
          and "resultType" not in legacy_list, detail)


SKILL_READ_ONLY = "skill documents are read-only over MCP"


def run_skill_write_guard_checks(admin, anon, csrf, mcp_auth, mcp_bin, sandbox, vault):
    """`type: skill` documents are instructions served to MCP clients, so no
    model-facing write path may create, change or attach to one. The web UI
    (admin session) still can."""
    csrf_hdr = {"X-CSRF-Token": csrf}
    skill_path, plain_path, flip_path = (
        "skills/guarded.md", "notes/guard-plain.md", "notes/guard-flip.md")
    for path, doc_type in ((skill_path, "skill"), (plain_path, "note"), (flip_path, "note")):
        status, _, body = admin.post_json(
            "/api/documents", {"path": path, "title": path, "body": "original", "type": doc_type},
            headers=csrf_hdr)
        check(f"web admin creates {path} (type {doc_type})", status in (200, 201),
              f"got {status} body={body[:200]!r}")

    def doc(path):
        status, _, body = admin.get(f"/api/documents/{path}")
        return json.loads(body) if status == 200 and body else {}

    def remote_tool(name, arguments):
        status, _, body = anon.post_json(
            "/mcp", {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                     "params": {"name": name, "arguments": arguments}},
            headers=mcp_auth)
        result = (json.loads(body) if body else {}).get("result") or {}
        text = (result.get("content") or [{}])[0].get("text", "")
        return status, result.get("isError") is True, text

    def refused(desc, outcome):
        status, is_error, text = outcome
        check(desc, status == 200 and is_error and SKILL_READ_ONLY in text,
              f"status={status} isError={is_error} text={text!r}")

    refused("remote create_document with type Skill is refused",
            remote_tool("create_document",
                        {"path": "skills/new.md", "title": "x", "body": "x", "type": "Skill"}))
    check("refused remote create left no file",
          not os.path.exists(os.path.join(vault, "skills", "new.md")))
    refused("remote update_document of a skill document is refused",
            remote_tool("update_document", {"path": skill_path, "body": "rewritten"}))
    refused("remote update_document turning a document into a skill is refused",
            remote_tool("update_document", {"path": plain_path, "type": "skill"}))
    check("refused remote updates changed nothing",
          doc(skill_path).get("body", "").strip() == "original"
          and doc(plain_path).get("type") == "note",
          f"skill={doc(skill_path)!r} plain={doc(plain_path)!r}")
    refused("remote attach_file to a skill document is refused",
            remote_tool("attach_file", {"path": skill_path, "filename": "a.txt",
                                        "content_base64": base64.b64encode(b"x").decode()}))
    status, is_error, text = remote_tool("attach_file_begin",
                                         {"path": skill_path, "filename": "a.txt"})
    check("remote attach_file_begin on a skill document is refused, no ticket issued",
          is_error and SKILL_READ_ONLY in text and "upload_id" not in text,
          f"status={status} text={text!r}")

    status, is_error, text = remote_tool("attach_file_begin",
                                         {"path": flip_path, "filename": "late.txt"})
    ticket = re.search(r"upload_id:\s*([0-9a-fA-F-]{36})", text)
    check("attach_file_begin on a plain document issues a ticket",
          not is_error and ticket is not None, f"text={text!r}")
    status, _, _ = admin.put_json(f"/api/documents/{flip_path}",
                                  {"title": flip_path, "body": "original", "type": "skill"},
                                  headers=csrf_hdr)
    check("web admin turns that document into a skill -> 200", status == 200, f"got {status}")
    status, _, body = anon.request("PUT", f"/mcp/uploads/{ticket.group(1) if ticket else 'x'}",
                                   body=b"late", headers={"Content-Type": "text/plain"})
    check("upload PUT to a document that became a skill after begin -> 403",
          status == 403 and not os.path.exists(os.path.join(vault, "notes", "guard-flip.assets")),
          f"got {status} body={body[:200]!r}")

    status, _, _ = admin.put_json(f"/api/documents/{skill_path}",
                                  {"title": skill_path, "body": "edited on the web", "type": "skill"},
                                  headers=csrf_hdr)
    check("web admin can still edit a skill document -> 200",
          status == 200 and doc(skill_path).get("body", "").strip() == "edited on the web",
          f"got {status}")

    status, _, audit = admin.get_json("/api/admin/mcp-audit-log")
    refusals = [e for e in (audit or {}).get("entries", [])
                if e.get("detail") == SKILL_READ_ONLY and not e.get("success")]
    check("every refused remote skill write is in the audit log",
          {e.get("toolName") for e in refusals} >= {
              "remote:create_document", "remote:update_document", "remote:attach_file",
              "remote:attach_file_begin"},
          f"refusals={[(e.get('toolName'), e.get('path')) for e in refusals]!r}")

    write_dir = os.path.join(sandbox, "mcp-write")
    os.makedirs(write_dir, exist_ok=True)
    with open(os.path.join(write_dir, "config.toml"), "w") as f:
        f.write(f"""
[vault]
path = "{vault}"
[index]
db_path = "{sandbox}/index.db"
[mcp]
scope = "admin"
write_access = true
[log]
level = "warn"
""")
    calls = [
        ("create_document", {"path": "skills/stdio.md", "title": "x", "body": "x",
                             "type": "skill"}),
        ("update_document", {"path": skill_path, "body": "stdio rewrite"}),
        ("update_document", {"path": plain_path, "type": " SKILL "}),
        ("attach_file", {"path": skill_path, "filename": "a.txt",
                         "content_base64": base64.b64encode(b"x").decode()}),
    ]
    stdio_input = "".join(
        json.dumps({"jsonrpc": "2.0", "id": i, "method": "tools/call",
                    "params": {"name": name, "arguments": args}}) + "\n"
        for i, (name, args) in enumerate(calls, start=1))
    stdio = subprocess.run([mcp_bin], cwd=write_dir, capture_output=True, text=True, timeout=20,
                           input=stdio_input)
    responses = {}
    for out_line in stdio.stdout.splitlines():
        msg = json.loads(out_line)
        responses[msg.get("id")] = msg.get("result") or {}
    for i, (name, args) in enumerate(calls, start=1):
        result = responses.get(i, {})
        text = (result.get("content") or [{}])[0].get("text", "")
        check(f"stdio {name} on/into a skill document is refused ({args.get('path')})",
              result.get("isError") is True and SKILL_READ_ONLY in text,
              f"result={result!r} stderr={stdio.stderr[-300:]!r}")
    check("refused stdio writes changed nothing",
          not os.path.exists(os.path.join(vault, "skills", "stdio.md"))
          and doc(skill_path).get("body", "").strip() == "edited on the web"
          and doc(plain_path).get("type") == "note",
          f"skill={doc(skill_path)!r} plain={doc(plain_path)!r}")


def run_mcp_skills_checks(admin, anon, csrf, mcp_auth, mcp_bin, sandbox, vault):
    """type: skill documents served through the MCP skills extension on the
    2026-07-28 path of both transports, with digests that match the bytes
    served and the same visibility gating as every tool."""
    csrf_hdr = {"X-CSRF-Token": csrf}
    for path, visibility, description in (
            ("skills/deploy_steps.md", "public", "Use when deploying: run the checklist"),
            ("skills/private-playbook.md", "private", "Private playbook")):
        status, _, body = admin.post_json(
            "/api/documents",
            {"path": path, "title": "Deploy Steps", "type": "skill", "visibility": visibility,
             "description": description, "body": "Read the attachment.\n"},
            headers=csrf_hdr)
        check(f"web admin creates skill document {path}", status in (200, 201),
              f"got {status} body={body[:200]!r}")
    status, _, body = admin.upload("/api/attachments/skills/deploy_steps.md", "check.png",
                                   b"\x89PNG\r\n\x1a\n-skill-asset", headers=csrf_hdr)
    check("web admin attaches a file to the public skill", status in (200, 201),
          f"got {status} body={body[:200]!r}")
    status, _, raw = admin.get("/api/documents/skills/deploy_steps.md")
    doc = json.loads(raw) if raw else {}
    check("documents API returns the description",
          doc.get("description") == "Use when deploying: run the checklist", f"doc={doc!r}")
    asset_link = f"![c](/assets/skills/deploy_steps.assets/check.png)"
    status, _, _ = admin.put_json(
        "/api/documents/skills/deploy_steps.md",
        {"title": "Deploy Steps", "type": "skill", "visibility": "public",
         "body": "Read the attachment.\n" + asset_link + "\n"},
        headers=csrf_hdr)
    status, _, raw = admin.get("/api/documents/skills/deploy_steps.md")
    check("update without description keeps it",
          status == 200
          and json.loads(raw).get("description") == "Use when deploying: run the checklist",
          f"got {status} body={raw[:200]!r}")

    def modern(method, params=None):
        p = dict(params or {})
        p["_meta"] = MODERN_MCP_META
        h = {"MCP-Protocol-Version": MODERN_MCP_VERSION, "Mcp-Method": method, **mcp_auth}
        if method == "resources/read":
            h["Mcp-Name"] = p.get("uri", "")
        status, _, body = anon.post_json(
            "/mcp", {"jsonrpc": "2.0", "id": 1, "method": method, "params": p}, headers=h)
        return status, (json.loads(body) if body else {}), body

    def read_bytes(rpc):
        contents = ((rpc.get("result") or {}).get("contents") or [{}])[0]
        if "text" in contents:
            return contents["text"].encode("utf-8")
        return base64.b64decode(contents.get("blob", ""))

    status, rpc, body = modern("server/discover")
    caps = (rpc.get("result") or {}).get("capabilities") or {}
    check("remote discover declares resources + the skills extension",
          "resources" in caps
          and "io.modelcontextprotocol/skills" in (caps.get("extensions") or {}),
          f"body={body[:300]!r}")

    status, rpc, body = modern("skills/list")
    result = rpc.get("result") or {}
    by_name = {s["frontmatter"]["name"]: s for s in result.get("skills") or []}
    check("remote skills/list lists both skills (admin scope) and skips invalid ones",
          status == 200 and result.get("resultType") == "complete"
          and set(by_name) == {"deploy-steps", "private-playbook"}
          and result.get("cacheScope") == "private" and isinstance(result.get("ttlMs"), int),
          f"status={status} body={body[:500]!r}")
    entry = by_name.get("deploy-steps") or {}
    check("skill entry: SKILL.md uri, verbatim frontmatter, every file listed",
          entry.get("uri") == "skill://deploy-steps/SKILL.md"
          and entry.get("frontmatter") == {"name": "deploy-steps",
                                           "description": "Use when deploying: run the checklist"}
          and [r["uri"] for r in entry.get("resources") or []]
          == ["skill://deploy-steps/SKILL.md", "skill://deploy-steps/assets/check.png"],
          f"entry={entry!r}")

    for res in entry.get("resources") or []:
        status, rpc, body = modern("resources/read", {"uri": res["uri"]})
        data = read_bytes(rpc)
        check(f"resources/read {res['uri']}: size and sha256 match the manifest",
              status == 200 and len(data) == res["size"]
              and res["digest"] == "sha256:" + hashlib.sha256(data).hexdigest(),
              f"status={status} size={len(data)} manifest={res!r} body={body[:200]!r}")
    status, rpc, _ = modern("resources/read", {"uri": "skill://deploy-steps/SKILL.md"})
    skill_md = read_bytes(rpc).decode("utf-8")
    check("SKILL.md: frontmatter block, title heading, asset link rewritten to assets/",
          skill_md.startswith('---\nname: "deploy-steps"\ndescription: "Use when deploying: run '
                              'the checklist"\n---\n')
          and "# Deploy Steps" in skill_md and "![c](assets/check.png)" in skill_md
          and "deploy_steps.assets" not in skill_md,
          f"SKILL.md={skill_md!r}")

    status, rpc, body = modern("skills/get", {"uri": "skill://deploy-steps/SKILL.md"})
    check("remote skills/get returns the same entry",
          status == 200 and (rpc.get("result") or {}).get("skill") == entry
          and (rpc.get("result") or {}).get("cacheScope") == "public",
          f"body={body[:300]!r}")
    for method, uri in (("skills/get", "skill://nope/SKILL.md"),
                        ("skills/get", "skill://deploy-steps/assets/check.png"),
                        ("resources/read", "skill://deploy-steps/../private-playbook/SKILL.md"),
                        ("resources/read", "skill://deploy-steps/assets/../../deploy_steps.md"),
                        ("resources/read", "skill://guarded/SKILL.md")):
        status, rpc, body = modern(method, {"uri": uri})
        check(f"remote {method} {uri} -> -32602",
              (rpc.get("error") or {}).get("code") == -32602, f"status={status} body={body[:200]!r}")

    status, rpc, body = modern("resources/list")
    uris = [r.get("uri") for r in (rpc.get("result") or {}).get("resources") or []]
    check("remote resources/list lists the skill files",
          "skill://deploy-steps/SKILL.md" in uris and "skill://deploy-steps/assets/check.png" in uris,
          f"body={body[:300]!r}")

    status, _, body = anon.post_json(
        "/mcp", {"jsonrpc": "2.0", "id": 1, "method": "skills/list"}, headers=mcp_auth)
    check("legacy (no _meta) skills/list is not served",
          (json.loads(body).get("error") or {}).get("code") == -32601 if body else False,
          f"status={status} body={body[:200]!r}")
    status, _, body = anon.post_json(
        "/mcp", {"jsonrpc": "2.0", "id": 1, "method": "initialize"}, headers=mcp_auth)
    legacy_caps = (json.loads(body).get("result") or {}).get("capabilities") or {}
    check("legacy initialize does not advertise resources", "resources" not in legacy_caps,
          f"caps={legacy_caps!r}")

    public_dir = os.path.join(sandbox, "mcp-public")
    os.makedirs(public_dir, exist_ok=True)
    with open(os.path.join(public_dir, "config.toml"), "w") as f:
        f.write(f"""
[vault]
path = "{vault}"
[index]
db_path = "{sandbox}/index.db"
[mcp]
scope = "public"
[log]
level = "warn"
""")

    def stdio_calls(cwd, calls):
        lines = "".join(
            json.dumps({"jsonrpc": "2.0", "id": i, "method": method,
                        "params": {**params, "_meta": MODERN_MCP_META}}) + "\n"
            for i, (method, params) in enumerate(calls, start=1))
        proc = subprocess.run([mcp_bin], cwd=cwd, capture_output=True, text=True, timeout=20,
                              input=lines)
        out = {}
        for line in proc.stdout.splitlines():
            msg = json.loads(line)
            out[msg.get("id")] = msg
        return out, f"stdout={proc.stdout[:500]!r} stderr={proc.stderr[-300:]!r}"

    responses, detail = stdio_calls(sandbox, [
        ("skills/list", {}),
        ("resources/read", {"uri": "skill://deploy-steps/assets/check.png"}),
    ])
    stdio_names = {s["frontmatter"]["name"]
                   for s in (responses.get(1, {}).get("result") or {}).get("skills") or []}
    stdio_entry = next((s for s in (responses.get(1, {}).get("result") or {}).get("skills") or []
                        if s["frontmatter"]["name"] == "deploy-steps"), {})
    check("stdio skills/list (scope admin) matches the remote entry",
          stdio_names == {"deploy-steps", "private-playbook"} and stdio_entry == entry, detail)
    check("stdio resources/read returns the asset bytes",
          read_bytes(responses.get(2, {})) == b"\x89PNG\r\n\x1a\n-skill-asset", detail)

    responses, detail = stdio_calls(public_dir, [
        ("skills/list", {}),
        ("skills/get", {"uri": "skill://private-playbook/SKILL.md"}),
        ("resources/read", {"uri": "skill://private-playbook/SKILL.md"}),
    ])
    public_result = responses.get(1, {}).get("result") or {}
    check("stdio scope=public: only the public skill, cacheScope public",
          {s["frontmatter"]["name"] for s in public_result.get("skills") or []} == {"deploy-steps"}
          and public_result.get("cacheScope") == "public", detail)
    check("stdio scope=public: private skill is unknown to skills/get and resources/read",
          (responses.get(2, {}).get("error") or {}).get("code") == -32602
          and (responses.get(3, {}).get("error") or {}).get("code") == -32602, detail)


def run_checks(sandbox, vault, server_bin):
    anon = Client(HOST, PORT)
    # `--version` prints "<name> <version>"; serverInfo.version must be the
    # bare version, since the name has its own serverInfo.name field.
    version_line = subprocess.run([server_bin, "--version"], capture_output=True,
                                  text=True, check=True).stdout.strip()
    expected_version = version_line.rsplit(" ", 1)[-1]
    if not re.fullmatch(r"\d+\.\d+\.\d+\S*", expected_version):
        raise RuntimeError(f"unexpected --version output: {version_line!r}")

    # --- 0. Shell routes always serve the same static page, no matter
    #        the path/query — the actual gating lives entirely in the
    #        JSON API now (see module docstring). Just confirm they don't
    #        error and don't reflect back any request data verbatim into
    #        the response (nothing here is per-request-rendered so there's
    #        no obvious injection surface, but worth a sanity check).
    for shell_path in ("/", "/login", "/search", "/folder", "/d/whatever.md",
                        "/edit/whatever.md", "/history/whatever.md"):
        status, _, _ = anon.get(shell_path)
        check(f"shell route {shell_path} -> 200", status == 200, f"got {status}")

    # --- 1. Unauthenticated writes must ALL be rejected ---------------
    status, _, _ = anon.post_json("/api/agent/sessions", {"instruction": "x"})
    check("anon agent start -> 401", status == 401, f"got {status}")
    status, _, _ = anon.get("/api/agent/sessions/not-a-real-id")
    check("anon agent poll -> 401", status == 401, f"got {status}")
    status, _, _ = anon.post_json("/api/agent/sessions/not-a-real-id/messages",
                                  {"instruction": "x"})
    check("anon agent follow-up -> 401", status == 401, f"got {status}")
    status, _, _ = anon.delete("/api/agent/sessions/not-a-real-id")
    check("anon agent drop -> 401", status == 401, f"got {status}")
    status, _, _ = anon.post_json("/api/agent/sessions/not-a-real-id/cancel", {})
    check("anon agent cancel -> 401", status == 401, f"got {status}")
    status, _, _ = anon.post_json("/api/documents", {"path": "x.md", "title": "x", "body": "y"})
    check("anon create -> 401", status == 401, f"got {status}")
    status, _, _ = anon.put_json("/api/documents/x.md", {"title": "x", "body": "y"})
    check("anon update -> 401", status == 401, f"got {status}")
    status, _, _ = anon.delete("/api/documents/x.md")
    check("anon delete -> 401", status == 401, f"got {status}")
    status, _, _ = anon.post_json("/api/documents/move", {"oldPath": "x.md", "newPath": "y.md"})
    check("anon document move -> 401", status == 401, f"got {status}")
    status, _, _ = anon.post_json("/api/documents/parse-import", {"content": "---\ntitle: x\n---\n"})
    check("anon parse-import -> 401", status == 401, f"got {status}")
    status, _, _ = anon.upload("/api/attachments/x.md", "a.png", b"data")
    check("anon upload -> 401", status == 401, f"got {status}")
    status, _, _ = anon.delete("/api/attachments/x.assets/a.png")
    check("anon delete attachment -> 401", status == 401, f"got {status}")
    check("vault has zero .md files after anon attempts",
          not any(f.endswith(".md") for _, _, files in os.walk(vault) for f in files))

    # Versioning routes (VersionRoutes.cpp) -- distinct URL prefixes from
    # /api/documents/{path...} on purpose (see that file's own comment on
    # why), which also means AuthFilter/CsrfFilter being LISTED on them
    # proves nothing on its own -- same discipline as every other
    # mutating route here, applied to the newest ones too.
    status, _, _ = anon.get("/api/document-history/x.md")
    check("anon document-history -> 401", status == 401, f"got {status}")
    status, _, _ = anon.post_json("/api/document-restore/x.md?id=1", {})
    check("anon document-restore -> 401", status == 401, f"got {status}")

    # MCP write-tool audit log (AdminRoutes.cpp) -- read-only but still
    # admin-only (it's a record of what an MCP client did to this vault,
    # not public information).
    status, _, _ = anon.get("/api/admin/mcp-audit-log")
    check("anon mcp-audit-log -> 401", status == 401, f"got {status}")

    # Vault backup download (AdminRoutes.cpp / BackupService.h) -- a plain
    # GET, but the full vault contents are exactly the kind of thing that
    # must never come back to an unauthenticated caller.
    status, _, _ = anon.get("/api/admin/backup")
    check("anon backup download -> 401", status == 401, f"got {status}")

    # Remote MCP transport (RemoteMcpRoutes.cpp) -- off by default, so an
    # anon POST to the actual /mcp endpoint (no bearer token needed to
    # provoke this -- it's not even reachable yet) must read as a plain
    # 404, indistinguishable from a route that was never registered, not
    # a 401 hinting "this exists, bring a token". The admin-only settings
    # routes underneath it (enable it, see/rotate the token, edit the
    # allowlist) need the same "no session -> 401" as everything else.
    status, _, _ = anon.post_json("/mcp", {"jsonrpc": "2.0", "id": 1, "method": "initialize"})
    check("anon /mcp while disabled -> 404 (not 401 -- no hint it exists)",
          status == 404, f"got {status}")
    status, _, _ = anon.request(
        "PUT", "/mcp/uploads/aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
        body=b"x", headers={"Content-Type": "application/octet-stream"})
    check("anon PUT /mcp/uploads while disabled -> 404 (not 401)",
          status == 404, f"got {status}")
    status, _, _ = anon.get("/api/admin/mcp-remote-config")
    check("anon GET mcp-remote-config -> 401", status == 401, f"got {status}")
    status, _, _ = anon.put_json("/api/admin/mcp-remote-config", {"enabled": True})
    check("anon PUT mcp-remote-config -> 401", status == 401, f"got {status}")
    status, _, _ = anon.get("/api/admin/mcp-remote-config/tokens")
    check("anon GET mcp-remote-config/tokens -> 401", status == 401, f"got {status}")
    status, _, _ = anon.post_json("/api/admin/mcp-remote-config/tokens", {"label": "x"})
    check("anon POST mcp-remote-config/tokens -> 401", status == 401, f"got {status}")
    status, _, _ = anon.post_json("/api/admin/mcp-remote-config/tokens/regenerate", {"id": 1})
    check("anon POST mcp-remote-config/tokens/regenerate -> 401", status == 401, f"got {status}")
    status, _, _ = anon.delete("/api/admin/mcp-remote-config/tokens?id=1")
    check("anon DELETE mcp-remote-config/tokens -> 401", status == 401, f"got {status}")
    status, _, _ = anon.post_json("/api/admin/mcp-remote-config/tokens/agent-access",
                                   {"id": 1, "enabled": True})
    check("anon POST mcp-remote-config/tokens/agent-access -> 401", status == 401, f"got {status}")
    status, _, _ = anon.post_json("/mcp/agent", {"messages": [{"role": "user", "content": "hi"}]})
    check("anon /mcp/agent while disabled -> 404 (not 401 -- no hint it exists)",
          status == 404, f"got {status}")
    status, _, _ = anon.post_json("/api/admin/mcp-remote-config/allowed-cidrs",
                                   {"cidr": "203.0.113.0/24"})
    check("anon POST allowed-cidrs -> 401", status == 401, f"got {status}")
    status, _, _ = anon.delete("/api/admin/mcp-remote-config/allowed-cidrs?cidr=203.0.113.0/24")
    check("anon DELETE allowed-cidrs -> 401", status == 401, f"got {status}")

    # --- 2. Session fixation: server never adopts a client-supplied ---
    #        session token; login always issues a fresh one -------------
    fixation = Client(HOST, PORT)
    fixation.cookies["wiki_session"] = "attacker-chosen-token-0000000000000000000000000000000000000000"
    status, _, _ = fixation.get("/login")
    pre_login_token = fixation.cookies.get("wiki_session")
    check("pre-set attacker token was not treated as authenticated",
          pre_login_token == "attacker-chosen-token-0000000000000000000000000000000000000000")
    status, _, _ = fixation.post_json("/api/login", {"username": "admin", "password": "SuperSecret123"})
    post_login_token = fixation.cookies.get("wiki_session")
    check("login issues a NEW session token, not the attacker-supplied one",
          post_login_token is not None and post_login_token != pre_login_token
          and post_login_token != "attacker-chosen-token-0000000000000000000000000000000000000000")

    # --- 3. Real admin session for the rest of the checks ---------------
    admin = Client(HOST, PORT)
    status, _, body = admin.get_json("/api/session")
    check("anon session check -> authenticated:false",
          body == {"authenticated": False, "agentEnabled": False}, f"got {body}")
    status, _, _ = admin.post_json("/api/login", {"username": "admin", "password": "SuperSecret123"})
    check("admin login -> 200", status == 200, f"got {status}")
    csrf = admin.cookies.get("wiki_csrf_token")
    check("csrf cookie set on login", csrf is not None)
    status, _, body = admin.get_json("/api/session")
    check("admin session check -> authenticated:true",
          body == {"authenticated": True, "agentEnabled": False}, f"got {body}")

    # --- 4. CSRF enforcement ---------------------------------------------
    status, _, _ = admin.put_json("/api/documents/nope.md", {"title": "x", "body": "y"})
    check("mutating request without csrf header -> 403", status == 403, f"got {status}")
    status, _, _ = admin.put_json(f"/api/documents/nope.md?csrf_token={csrf}",
                                   {"title": "x", "body": "y"})
    check("csrf token in the query string instead of the header -> 403",
          status == 403, f"got {status}")
    status, _, _ = admin.post_json("/api/documents/move", {"oldPath": "x.md", "newPath": "y.md"})
    check("document move without csrf header -> 403", status == 403, f"got {status}")
    status, _, _ = admin.post_json("/api/documents/parse-import", {"content": "x"})
    check("parse-import without csrf header -> 403", status == 403, f"got {status}")
    imported_cases = [
        ("single-quoted apostrophe", "---\ntitle: 'Note: it''s here'\n---\nBody\n",
         {"title": "Note: it's here"}, "Body\n"),
        ("double-quoted \\n escape", '---\ntitle: "line1\\nline2"\n---\nBody\n',
         {"title": "line1\nline2"}, "Body\n"),
        ("CRLF line endings", "---\r\ntitle: Hi\r\ntags: [a]\r\n---\r\nBody\r\n",
         {"title": "Hi", "tags": ["a"]}, "Body\r\n"),
        ("block-style tags", "---\ntags:\n  - a\n  - b\n---\nBody\n",
         {"tags": ["a", "b"]}, "Body\n"),
        ("no front matter", "Just text\n", {}, "Just text\n"),
    ]
    for label, content, want_fields, want_body in imported_cases:
        status, _, raw = admin.post_json("/api/documents/parse-import", {"content": content},
                                          headers={"X-CSRF-Token": csrf})
        got = json.loads(raw) if raw else None
        check(f"parse-import: {label}",
              status == 200 and got == {"fields": want_fields, "body": want_body},
              f"got {status} {got}")
    status, _, _ = admin.post_json("/api/documents/parse-import", {"content": 5},
                                    headers={"X-CSRF-Token": csrf})
    check("parse-import: non-string content -> 400", status == 400, f"got {status}")
    status, _, _ = admin.post_json("/api/agent/sessions", {"instruction": "x"})
    check("agent start without csrf header -> 403", status == 403, f"got {status}")
    status, _, raw = admin.post_json("/api/agent/sessions", {"instruction": "x"},
                                      headers={"X-CSRF-Token": csrf})
    body = json.loads(raw) if raw else None
    check("agent start while unconfigured -> 404",
          status == 404 and body and body.get("error") == "agent not configured",
          f"got {status} {body}")
    status, _, _ = admin.post_json("/api/agent/sessions/not-a-real-id/cancel", {})
    check("agent cancel without csrf header -> 403", status == 403, f"got {status}")
    status, _, raw = admin.post_json("/api/agent/sessions/not-a-real-id/cancel", {},
                                      headers={"X-CSRF-Token": csrf})
    body = json.loads(raw) if raw else None
    check("agent cancel while unconfigured -> 404",
          status == 404 and body and body.get("error") == "agent not configured",
          f"got {status} {body}")

    # --- 5. Path traversal ------------------------------------------------
    # The shell route itself touches no filesystem (see check 0) — the
    # real read goes through the JSON API, which still has to reject this.
    status, _, body = anon.get("/api/documents/../../../etc/passwd")
    check("path traversal on /api/documents/{path} -> 400/404, not leaked",
          status in (400, 404) and b"root:" not in body, f"got {status}")
    status, _, _ = admin.post_json("/api/documents", {"path": "../../etc/evil.md", "title": "x", "body": "y"},
                                    headers={"X-CSRF-Token": csrf})
    check("path traversal in create payload -> 400", status == 400, f"got {status}")

    # --- 6. Full CRUD + visibility gating, across every surface --------
    status, _, _ = admin.post_json(
        "/api/documents",
        {"path": "notes/public.md", "title": "Public Doc", "tags": ["e2e"],
         "visibility": "public", "type": "note", "body": "systemd public content"},
        headers={"X-CSRF-Token": csrf})
    check("create public doc -> 201", status == 201, f"got {status}")

    status, _, _ = admin.post_json(
        "/api/documents",
        {"path": "notes/private.md", "title": "Private Doc", "tags": ["e2e"],
         "visibility": "private", "type": "note", "body": "systemd private content"},
        headers={"X-CSRF-Token": csrf})
    check("create private doc -> 201", status == 201, f"got {status}")

    status, _, _ = anon.get("/api/documents/notes/public.md")
    check("anon sees public doc via JSON API", status == 200, f"got {status}")
    status, _, _ = anon.get("/api/documents/notes/private.md")
    check("anon private doc -> 404 (not 403)", status == 404, f"got {status}")
    status, _, _ = admin.get("/api/documents/notes/private.md")
    check("admin sees private doc", status == 200, f"got {status}")

    # ?content= picks one representation of the document body; without it
    # the response keeps both, and every variant keeps the same gating.
    status, _, doc = anon.get_json("/api/documents/notes/public.md")
    check("no ?content: response has both body and renderedHtml",
          status == 200 and "body" in doc and "renderedHtml" in doc, f"status={status}")
    status, _, doc = anon.get_json("/api/documents/notes/public.md?content=html")
    check("?content=html: renderedHtml only, metadata kept",
          status == 200 and "body" not in doc and "systemd public content" in doc.get("renderedHtml", "")
          and doc.get("title") == "Public Doc", f"status={status} keys={sorted(doc or {})}")
    status, _, doc = anon.get_json("/api/documents/notes/public.md?content=markdown")
    check("?content=markdown: body only, metadata kept",
          status == 200 and "renderedHtml" not in doc and "systemd public content" in doc.get("body", "")
          and doc.get("title") == "Public Doc", f"status={status} keys={sorted(doc or {})}")
    for content in ("html", "markdown"):
        status, _, _ = anon.get(f"/api/documents/notes/private.md?content={content}")
        check(f"anon private doc with ?content={content} -> 404", status == 404, f"got {status}")
        status, _, _ = admin.get(f"/api/documents/notes/private.md?content={content}")
        check(f"admin private doc with ?content={content} -> 200", status == 200, f"got {status}")
    status_private, _, body_private = anon.get("/api/documents/notes/private.md?content=xml")
    status_missing, _, body_missing = anon.get("/api/documents/notes/does-not-exist.md?content=xml")
    check("unknown ?content -> 400, identical for a private and a missing doc",
          status_private == 400 and status_missing == 400 and body_private == body_missing,
          f"private={status_private} {body_private[:80]!r} missing={status_missing}")

    status, _, body = admin.post_json(
        "/api/documents",
        {"path": "notes/no-suffix", "title": "No Suffix", "visibility": "public",
         "body": "created without .md"},
        headers={"X-CSRF-Token": csrf})
    check("create without .md suffix -> 201", status == 201, f"got {status}")
    nosuffix = json.loads(body.decode()) if body else {}
    check("create without .md returns path with .md",
          nosuffix.get("path") == "notes/no-suffix.md", f"got {nosuffix}")
    status, _, _ = admin.get("/api/documents/notes/no-suffix.md")
    check("appended-.md document is readable", status == 200, f"got {status}")

    status, _, body = admin.post_json(
        "/api/documents/move",
        {"oldPath": "notes/no-suffix.md", "newPath": "../../etc/evil.md"},
        headers={"X-CSRF-Token": csrf})
    check("path traversal in document move -> 400, not leaked",
          status == 400 and b"root:" not in body and b"/home/" not in body,
          f"got {status} {body[:200]!r}")
    status, _, _ = admin.get("/api/documents/notes/no-suffix.md")
    check("document still in place after rejected move", status == 200, f"got {status}")

    # --- 6a2. /api/documents/{path}/raw (literal on-disk bytes) -----------
    # This route must be registered BEFORE the general
    # "^/api/documents/(.*)$" handler in DocumentRoutes.cpp -- Drogon
    # matches regex handlers in registration order, and the general
    # handler's own "(.*)" would otherwise greedily swallow a trailing
    # "/raw" as part of its own docPath, making this route unreachable.
    status, _, body = anon.get("/api/documents/notes/public.md/raw")
    text = body.decode("utf-8")
    check("anon raw: public doc returns literal file bytes (front-matter + body), not JSON",
          status == 200 and "systemd public content" in text and text.lstrip().startswith("---"),
          f"status={status} body={text[:120]!r}")
    status, _, _ = anon.get("/api/documents/notes/private.md/raw")
    check("anon raw: private doc -> 404 (not 403)", status == 404, f"got {status}")
    status, _, body = admin.get("/api/documents/notes/private.md/raw")
    check("admin raw: sees private doc's literal bytes",
          status == 200 and "systemd private content" in body.decode("utf-8"), f"status={status}")

    status, _, body = anon.get_json("/api/search?q=systemd")
    paths = [r["path"] for r in body["results"]]
    check("anon search: public found, private not leaked",
          "notes/public.md" in paths and "notes/private.md" not in paths, f"paths={paths}")
    status, _, body = admin.get_json("/api/search?q=systemd")
    paths = [r["path"] for r in body["results"]]
    check("admin search: sees both", "notes/private.md" in paths, f"paths={paths}")

    status, _, body = anon.get("/api/nav/tree")
    tree = json.loads(body)
    check("anon nav tree excludes private", "notes/private.md" not in [d["path"] for d in tree])
    status, _, body = admin.get("/api/nav/tree")
    tree = json.loads(body)
    check("admin nav tree includes private", "notes/private.md" in [d["path"] for d in tree])

    status, _, body = anon.get("/api/nav/tags")
    anon_tags = {t["tag"]: t["count"] for t in json.loads(body)}
    status, _, body = admin.get("/api/nav/tags")
    admin_tags = {t["tag"]: t["count"] for t in json.loads(body)}
    check("tag count differs by scope (private doc contributes only for admin)",
          anon_tags.get("e2e", 0) == 1 and admin_tags.get("e2e", 0) == 2,
          f"anon={anon_tags} admin={admin_tags}")

    # --- 6b. /api/query (```query block engine) -------------------------
    status, _, body = anon.get_json("/api/query?q=" + urllib.parse.quote("tag: e2e"))
    paths = [r["path"] for r in body["rows"]]
    check("anon query: public found, private not leaked",
          "notes/public.md" in paths and "notes/private.md" not in paths, f"paths={paths}")
    status, _, body = admin.get_json("/api/query?q=" + urllib.parse.quote("tag: e2e"))
    paths = [r["path"] for r in body["rows"]]
    check("admin query: sees both", "notes/private.md" in paths, f"paths={paths}")

    # A typo'd key must be a clear 400 error, never a silently empty or
    # unfiltered 200 -- see QueryBlocks.h's own reasoning for why.
    status, _, body = admin.get_json("/api/query?q=" + urllib.parse.quote("tags: e2e"))
    check("query: unknown key -> 400 with an error field, not a silent empty result",
          status == 400 and "error" in body, f"status={status} body={body}")

    # A tag value shaped like a SQL injection attempt must be treated as
    # an inert literal (bound parameter), never concatenated into SQL --
    # the endpoint must still respond normally (200, no match), not 500.
    injection = "tag: x'; DROP TABLE documents; --"
    status, _, body = admin.get_json("/api/query?q=" + urllib.parse.quote(injection))
    check("query: SQL-injection-shaped tag value is inert, not a 500",
          status == 200 and body["rows"] == [], f"status={status} body={body}")
    # The documents table must still be intact and queryable afterward.
    status, _, body = admin.get_json("/api/query?q=" + urllib.parse.quote("tag: e2e"))
    check("query: documents table still intact after the injection attempt",
          status == 200 and len(body["rows"]) == 2, f"status={status} body={body}")

    # --- 6b2. /api/query `search:` (delegates to the same FtsSearch engine
    # /api/search uses -- same visibility gating must hold through the
    # delegation, not just in FtsSearch's own direct callers) -----------
    status, _, body = anon.get_json("/api/query?q=" + urllib.parse.quote("search: systemd"))
    paths = [r["path"] for r in body["rows"]]
    check("anon query search: public found, private not leaked",
          "notes/public.md" in paths and "notes/private.md" not in paths, f"paths={paths}")
    status, _, body = admin.get_json("/api/query?q=" + urllib.parse.quote("search: systemd"))
    paths = [r["path"] for r in body["rows"]]
    check("admin query search: sees both", "notes/private.md" in paths, f"paths={paths}")

    # search: combined with sort/order/orphans is an explicit parse error,
    # not one silently overriding the other.
    status, _, body = admin.get_json(
        "/api/query?q=" + urllib.parse.quote("search: systemd\nsort: title"))
    check("query: search+sort -> 400 with an error field",
          status == 400 and "error" in body, f"status={status} body={body}")

    # --- 6c. /api/graph (full/local graph data source) -------------------
    status, _, _ = admin.post_json(
        "/api/documents",
        {"path": "notes/graph-pub-source.md", "title": "Graph Pub Source", "tags": [],
         "visibility": "public", "type": "note",
         "body": "Links to [[notes/graph-priv-target]]."},
        headers={"X-CSRF-Token": csrf})
    check("create graph pub-source doc -> 201", status == 201, f"got {status}")
    status, _, _ = admin.post_json(
        "/api/documents",
        {"path": "notes/graph-priv-target.md", "title": "Graph Priv Target", "tags": [],
         "visibility": "private", "type": "note", "body": "private target"},
        headers={"X-CSRF-Token": csrf})
    check("create graph priv-target doc -> 201", status == 201, f"got {status}")

    status, _, body = anon.get_json("/api/graph")
    anon_paths = [n["path"] for n in body["nodes"]]
    anon_edges = [(e["source"], e["target"]) for e in body["edges"]]
    check("anon graph: public source node present, private target node absent",
          "notes/graph-pub-source.md" in anon_paths and
          "notes/graph-priv-target.md" not in anon_paths, f"paths={anon_paths}")
    check("anon graph: edge touching the private document is not leaked",
          ("notes/graph-pub-source.md", "notes/graph-priv-target.md") not in anon_edges,
          f"edges={anon_edges}")

    status, _, body = admin.get_json("/api/graph")
    admin_paths = [n["path"] for n in body["nodes"]]
    admin_edges = [(e["source"], e["target"]) for e in body["edges"]]
    check("admin graph: both nodes present",
          "notes/graph-pub-source.md" in admin_paths and
          "notes/graph-priv-target.md" in admin_paths, f"paths={admin_paths}")
    check("admin graph: the edge is present",
          ("notes/graph-pub-source.md", "notes/graph-priv-target.md") in admin_edges,
          f"edges={admin_edges}")

    # --- 6c2. GET /api/graph?around= (connected component) -------------
    # PathGuard + fail-safe-private, depth is the full visible component
    # (not 1-hop, not a client hops=), exact bind (not LIKE). Same
    # 404-not-403 as GET /api/documents for private/missing/traversal,
    # and an edge still requires BOTH ends visible. A private document
    # is not a stepping stone to a further public one.
    status, _, _ = admin.post_json(
        "/api/documents",
        {"path": "notes/around-center.md", "title": "Around Center", "tags": [],
         "visibility": "public", "type": "note",
         "body": "Links to [[notes/around-pub]] and [[notes/around-priv]]."},
        headers={"X-CSRF-Token": csrf})
    check("create around-center doc -> 201", status == 201, f"got {status}")
    status, _, _ = admin.post_json(
        "/api/documents",
        {"path": "notes/around-pub.md", "title": "Around Pub", "tags": [],
         "visibility": "public", "type": "note",
         "body": "Links to [[notes/around-twohop]]."},
        headers={"X-CSRF-Token": csrf})
    check("create around-pub doc -> 201", status == 201, f"got {status}")
    status, _, _ = admin.post_json(
        "/api/documents",
        {"path": "notes/around-priv.md", "title": "Around Priv", "tags": [],
         "visibility": "private", "type": "note", "body": "private neighbor"},
        headers={"X-CSRF-Token": csrf})
    check("create around-priv doc -> 201", status == 201, f"got {status}")
    status, _, _ = admin.post_json(
        "/api/documents",
        {"path": "notes/around-twohop.md", "title": "Around Twohop", "tags": [],
         "visibility": "public", "type": "note", "body": "two hops from center"},
        headers={"X-CSRF-Token": csrf})
    check("create around-twohop doc -> 201", status == 201, f"got {status}")

    around_center = "/api/graph?around=" + urllib.parse.quote(
        "notes/around-center.md", safe="")
    status, _, body = anon.get_json(around_center)
    anon_around_paths = [n["path"] for n in body["nodes"]]
    anon_around_edges = [(e["source"], e["target"]) for e in body["edges"]]
    check("anon around=center: public neighbor present, private not leaked",
          "notes/around-center.md" in anon_around_paths and
          "notes/around-pub.md" in anon_around_paths and
          "notes/around-priv.md" not in anon_around_paths,
          f"paths={anon_around_paths}")
    check("anon around=center: public two-hop (via public neighbor) is included",
          "notes/around-twohop.md" in anon_around_paths,
          f"paths={anon_around_paths}")
    check("anon around=center: public two-hop edge is present",
          ("notes/around-pub.md", "notes/around-twohop.md") in anon_around_edges,
          f"edges={anon_around_edges}")
    check("anon around=center: public edge present, private-touching edge absent",
          ("notes/around-center.md", "notes/around-pub.md") in anon_around_edges and
          ("notes/around-center.md", "notes/around-priv.md") not in anon_around_edges,
          f"edges={anon_around_edges}")

    # hops= from the client is ignored — depth is the full component, so
    # hops=0 must NOT shrink the result back to just the center.
    status, _, body = anon.get_json(around_center + "&hops=0")
    hops_paths = [n["path"] for n in body["nodes"]]
    check("anon around=center&hops=0 still includes the two-hop document",
          "notes/around-twohop.md" in hops_paths, f"paths={hops_paths}")

    status, _, body = anon.get(
        "/api/graph?around=" + urllib.parse.quote("notes/around-priv.md", safe=""))
    check("anon around=private -> 404, not 403",
          status == 404, f"got {status}")
    check("anon around=private 404 body does not leak the path",
          b"around-priv" not in body, f"body={body[:200]!r}")

    status, _, body = admin.get_json(
        "/api/graph?around=" + urllib.parse.quote("notes/around-priv.md", safe=""))
    admin_priv_paths = [n["path"] for n in body["nodes"]]
    admin_priv_edges = [(e["source"], e["target"]) for e in body["edges"]]
    check("admin around=private: center and its public source are present",
          "notes/around-priv.md" in admin_priv_paths and
          "notes/around-center.md" in admin_priv_paths, f"paths={admin_priv_paths}")
    check("admin around=private: the inbound edge is present",
          ("notes/around-center.md", "notes/around-priv.md") in admin_priv_edges,
          f"edges={admin_priv_edges}")

    # Private document as a stepping stone: A(public)→B(private)→C(public)
    # must not reveal C to an anonymous around=A (they cannot see B, so
    # they cannot walk through it).
    status, _, _ = admin.post_json(
        "/api/documents",
        {"path": "notes/around-step-a.md", "title": "Step A", "tags": [],
         "visibility": "public", "type": "note",
         "body": "Links to [[notes/around-step-b]]."},
        headers={"X-CSRF-Token": csrf})
    check("create around-step-a doc -> 201", status == 201, f"got {status}")
    status, _, _ = admin.post_json(
        "/api/documents",
        {"path": "notes/around-step-b.md", "title": "Step B", "tags": [],
         "visibility": "private", "type": "note",
         "body": "Links to [[notes/around-step-c]]."},
        headers={"X-CSRF-Token": csrf})
    check("create around-step-b doc -> 201", status == 201, f"got {status}")
    status, _, _ = admin.post_json(
        "/api/documents",
        {"path": "notes/around-step-c.md", "title": "Step C", "tags": [],
         "visibility": "public", "type": "note", "body": "only reachable via private B"},
        headers={"X-CSRF-Token": csrf})
    check("create around-step-c doc -> 201", status == 201, f"got {status}")

    status, _, body = anon.get_json(
        "/api/graph?around=" + urllib.parse.quote("notes/around-step-a.md", safe=""))
    step_paths = [n["path"] for n in body["nodes"]]
    check("anon around=step-a does not walk through private B to public C",
          "notes/around-step-a.md" in step_paths and
          "notes/around-step-b.md" not in step_paths and
          "notes/around-step-c.md" not in step_paths,
          f"paths={step_paths}")
    status, _, body = admin.get_json(
        "/api/graph?around=" + urllib.parse.quote("notes/around-step-a.md", safe=""))
    admin_step_paths = [n["path"] for n in body["nodes"]]
    check("admin around=step-a walks through private B to public C",
          "notes/around-step-a.md" in admin_step_paths and
          "notes/around-step-b.md" in admin_step_paths and
          "notes/around-step-c.md" in admin_step_paths,
          f"paths={admin_step_paths}")

    status, _, body = anon.get(
        "/api/graph?around=" + urllib.parse.quote("../../../etc/passwd"))
    check("around= path traversal -> 404, passwd contents not leaked",
          status == 404 and b"root:" not in body, f"got {status} body={body[:200]!r}")
    check("around= path traversal 404 body does not echo the payload",
          b"passwd" not in body and b"etc" not in body, f"body={body[:200]!r}")

    # A LIKE-wildcard around= must be an exact miss (404), not a dump of
    # every document whose path happens to match.
    status, _, body = anon.get("/api/graph?around=" + urllib.parse.quote("%"))
    check("around=% is a literal miss, not a LIKE pattern",
          status == 404, f"got {status}")

    injection = "x'; DROP TABLE documents; --"
    status, _, _ = anon.get("/api/graph?around=" + urllib.parse.quote(injection))
    check("around= SQL-injection-shaped path is a 404, not a 500",
          status == 404, f"got {status}")
    status, _, body = admin.get_json("/api/graph")
    check("around= injection attempt did not drop the documents table",
          status == 200 and "notes/around-center.md" in [n["path"] for n in body["nodes"]],
          f"status={status} nodes={len(body.get('nodes', [])) if isinstance(body, dict) else 'n/a'}")

    # --- 6c3. GET /api/graph/matches?q= (FTS paths for graph filter) ---
    # Same visibility gate as /api/search: a private body's unique token
    # must not appear in an anonymous path list. Empty q is an empty
    # list, not every document. Paths only — no snippets.
    status, _, _ = admin.post_json(
        "/api/documents",
        {"path": "notes/graph-match-pub.md", "title": "Graph Match Pub", "tags": [],
         "visibility": "public", "type": "note",
         "body": "graphmatchxyzzyquux appears only in this public body."},
        headers={"X-CSRF-Token": csrf})
    check("create graph-match public doc -> 201", status == 201, f"got {status}")
    status, _, _ = admin.post_json(
        "/api/documents",
        {"path": "notes/graph-match-priv.md", "title": "Graph Match Priv", "tags": [],
         "visibility": "private", "type": "note",
         "body": "graphmatchxyzzyquux also appears in this private body."},
        headers={"X-CSRF-Token": csrf})
    check("create graph-match private doc -> 201", status == 201, f"got {status}")

    status, _, body = anon.get_json(
        "/api/graph/matches?q=" + urllib.parse.quote("graphmatchxyzzyquux"))
    anon_match = body.get("paths", []) if isinstance(body, dict) else []
    check("anon graph/matches: public body hit, private path absent",
          "notes/graph-match-pub.md" in anon_match and
          "notes/graph-match-priv.md" not in anon_match,
          f"paths={anon_match}")
    check("anon graph/matches body has paths only, no snippets",
          isinstance(body, dict) and "results" not in body and "snippet" not in str(body),
          f"body keys={list(body) if isinstance(body, dict) else type(body)}")

    status, _, body = admin.get_json(
        "/api/graph/matches?q=" + urllib.parse.quote("graphmatchxyzzyquux"))
    admin_match = body.get("paths", []) if isinstance(body, dict) else []
    check("admin graph/matches: both public and private body hits",
          "notes/graph-match-pub.md" in admin_match and
          "notes/graph-match-priv.md" in admin_match,
          f"paths={admin_match}")

    status, _, body = anon.get_json("/api/graph/matches")
    empty_match = body.get("paths", None) if isinstance(body, dict) else None
    check("anon graph/matches with empty q is an empty list, not every document",
          empty_match == [], f"paths={empty_match}")

    # --- 6d. /api/calendar (due/recur, CalendarQueries) -------------------
    status, _, _ = admin.post_json(
        "/api/documents",
        {"path": "notes/cal-pub.md", "title": "Cal Pub", "tags": [],
         "visibility": "public", "type": "note", "body": "public event",
         "due": "2026-06-01", "recur": "weekly"},
        headers={"X-CSRF-Token": csrf})
    check("create public recurring event -> 201", status == 201, f"got {status}")
    status, _, _ = admin.post_json(
        "/api/documents",
        {"path": "notes/cal-priv.md", "title": "Cal Priv", "tags": [],
         "visibility": "private", "type": "note", "body": "private event",
         "due": "2026-06-02", "recur": ""},
        headers={"X-CSRF-Token": csrf})
    check("create private one-off event -> 201", status == 201, f"got {status}")

    # Calendar is admin-only outright, not fail-safe-private-per-document
    # like every other read route here (search, nav, query blocks) —
    # decided live after noticing a PUBLIC document's due date was still
    # visible to an anonymous caller via /api/calendar: that's the
    # admin's actual schedule, not just "this one document happens to be
    # public", so even the public event must not leak to an anonymous
    # caller. See CalendarRoutes.cpp's own comment on this exception.
    status, _, body = anon.get_json("/api/calendar?start=2026-06-01&end=2026-06-30")
    check("anon calendar -> 401, not a filtered (even partial) event list",
          status == 401 and "error" in body, f"status={status} body={body}")

    status, _, body = admin.get_json("/api/calendar?start=2026-06-01&end=2026-06-30")
    admin_paths = [e["path"] for e in body["events"]]
    check("admin calendar: sees both the public and the private event",
          "notes/cal-pub.md" in admin_paths and "notes/cal-priv.md" in admin_paths,
          f"paths={admin_paths}")
    # Weekly from 2026-06-01 (a Monday) within June must expand to more
    # than one occurrence, not just echo the bare due date back once --
    # this is the one thing a plain visibility-filtered SELECT could never
    # produce on its own.
    admin_dates = sorted(e["date"] for e in body["events"] if e["path"] == "notes/cal-pub.md")
    check("admin calendar: weekly recurrence expands to multiple June occurrences",
          len(admin_dates) >= 4, f"dates={admin_dates}")

    status, _, body = admin.get_json("/api/calendar?start=2026-06-01")
    check("calendar: missing 'end' -> 400 with an error field, not a silent empty result",
          status == 400 and "error" in body, f"status={status} body={body}")

    # --- 7. Attachments: visibility follows the OWNING document --------
    # No extension policy on upload anymore (see AttachmentService) — an
    # extension that would have been rejected before (.exe) now succeeds;
    # the safety boundary moved to the SERVING side instead (forced
    # download for anything not on a small inline-safe allowlist), which
    # doesn't change any status code this script checks.
    status, _, body = admin.upload("/api/attachments/notes/private.md", "secret.png", b"fake png bytes",
                                    headers={"X-CSRF-Token": csrf})
    check("upload attachment to private doc -> 201", status == 201, f"got {status}")
    attach_path = json.loads(body)["path"]
    status, _, _ = admin.get(f"/assets/{attach_path}")
    check("admin can fetch attachment of private doc", status == 200, f"got {status}")
    status, _, _ = anon.get(f"/assets/{attach_path}")
    check("anon CANNOT fetch attachment of private doc", status == 404, f"got {status}")

    # Non-ASCII filenames keep their letters, both through the URL-encoded
    # /assets/ path and in a forced download's Content-Disposition.
    status, _, body = admin.upload("/api/attachments/notes/private.md", "Схема опис.docx",
                                    b"docx bytes", headers={"X-CSRF-Token": csrf})
    check("upload Cyrillic-named attachment -> 201", status == 201, f"got {status}")
    cyrillic_path = json.loads(body)["path"]
    check("Cyrillic filename is stored with its letters",
          cyrillic_path == "notes/private.assets/Схема_опис.docx", f"got {cyrillic_path!r}")
    status, hdrs, content = admin.get("/assets/" + urllib.parse.quote(cyrillic_path))
    check("Cyrillic-named attachment is served", status == 200 and content == b"docx bytes",
          f"got {status}")
    disposition = {k.lower(): v for k, v in hdrs.items()}.get("content-disposition", "")
    check("forced download carries an RFC 5987 UTF-8 filename",
          disposition == "attachment; filename=\"" + "_" * 19 + ".docx\"; filename*=UTF-8''"
          + urllib.parse.quote("Схема_опис.docx", safe=""),
          f"got {disposition!r}")

    # A markdown attachment carrying its own `visibility: public` front
    # matter is still an attachment of a private document, not a document.
    status, _, body = admin.upload(
        "/api/attachments/notes/private.md", "smuggled.md",
        b"---\ntitle: Smuggled\nvisibility: public\n---\nsmuggleprobe body\n",
        headers={"X-CSRF-Token": csrf})
    check("upload markdown attachment to private doc -> 201", status == 201, f"got {status}")
    smuggled_path = json.loads(body)["path"]
    time.sleep(1.0)  # VaultWatcher debounce
    for who, client in (("anon", anon), ("admin", admin)):
        status, _, _ = client.get(f"/api/documents/{smuggled_path}")
        check(f"{who} cannot read a markdown attachment as a document",
              status == 404, f"got {status}")
        status, _, results = client.get_json("/api/search?q=smuggleprobe")
        check(f"{who} search does not list a markdown attachment",
              results["results"] == [], f"got {results!r}")
    status, _, _ = anon.get(f"/assets/{smuggled_path}")
    check("anon CANNOT fetch markdown attachment of private doc", status == 404, f"got {status}")
    status, _, _ = admin.get(f"/assets/{smuggled_path}")
    check("admin can still fetch the markdown attachment as an asset",
          status == 200, f"got {status}")

    # Requesting the .assets DIRECTORY itself (not a file inside it) must
    # 404, not crash the server — newFileResponse()/sendfile() on a
    # directory fd is a Drogon FATAL, not a normal error path. The
    # trailing slash is what makes owningDocumentFor() match the
    # directory's own name as if it were an asset path.
    assets_dir = attach_path.rsplit("/", 1)[0]
    status, _, _ = admin.get(f"/assets/{assets_dir}/")
    check("fetching the .assets directory itself -> 404, not a crash",
          status == 404, f"got {status}")
    status, _, _ = admin.get("/healthz")
    check("server still alive after directory-as-file request", status == 200, f"got {status}")

    status, _, body = admin.get_json("/api/attachments/notes/private.md")
    listed = [f["path"] for f in (body or {}).get("files", [])] if isinstance(body, dict) else []
    check("admin lists attachments of private doc",
          attach_path in listed, f"listed={listed}")
    status, _, _ = anon.get("/api/attachments/notes/private.md")
    check("anon list attachments of private doc -> 404 (not 403)",
          status == 404, f"got {status}")
    status, _, _ = admin.upload("/api/attachments/notes/private.md", "extra.bin", b"extra",
                                headers={"X-CSRF-Token": csrf})
    check("upload second attachment -> 201", status == 201, f"got {status}")
    extra_path = "notes/private.assets/extra.bin"
    status, _, _ = admin.delete(f"/api/attachments/{extra_path}")
    check("mutating delete attachment without csrf -> 403", status == 403, f"got {status}")
    status, _, _ = admin.delete(f"/api/attachments/{extra_path}",
                                 headers={"X-CSRF-Token": csrf})
    check("admin delete attachment -> 200", status == 200, f"got {status}")
    status, _, body = admin.get_json("/api/attachments/notes/private.md")
    listed = [f["path"] for f in (body or {}).get("files", [])] if isinstance(body, dict) else []
    check("deleted attachment gone from list, original remains",
          extra_path not in listed and attach_path in listed, f"listed={listed}")
    status, _, _ = admin.delete("/api/attachments/notes/private.md",
                                 headers={"X-CSRF-Token": csrf})
    check("delete owning-document path as if it were an asset -> 400",
          status == 400, f"got {status}")

    # --- 7b. Remote MCP large-file upload (attach_file_begin + PUT) ------
    # Bytes must not travel as JSON/base64 (that would blow the model
    # context). attach_file_begin issues a UUID ticket; PUT /mcp/uploads/{id}
    # then accepts the raw body with no Bearer — the id is the capability.
    status, _, body = admin.post_json(
        "/api/admin/mcp-remote-config/tokens", {"label": "e2e-primary"},
        headers={"X-CSRF-Token": csrf})
    check("create remote MCP token -> 200", status == 200, f"got {status}")
    created_primary = json.loads(body)
    mcp_token, mcp_token_id = created_primary.get("token"), created_primary.get("id")
    check("create token returns a raw token once",
          isinstance(mcp_token, str) and len(mcp_token) >= 32, f"got {mcp_token!r}")
    status, _, _ = admin.put_json(
        "/api/admin/mcp-remote-config",
        {"enabled": True, "writeEnabled": True},
        headers={"X-CSRF-Token": csrf})
    check("enable remote MCP writes -> 200", status == 200, f"got {status}")

    mcp_auth = {"Authorization": f"Bearer {mcp_token}"}

    # IP allowlist is checked before the token: from outside it, a valid
    # and an invalid token are indistinguishable and neither is "used".
    def token_last_used():
        _, _, listing = admin.get_json("/api/admin/mcp-remote-config/tokens")
        return next(t["lastUsedAt"] for t in listing["tokens"] if t["id"] == mcp_token_id)

    last_used_before = token_last_used()
    status, _, _ = admin.post_json("/api/admin/mcp-remote-config/allowed-cidrs",
                                    {"cidr": "203.0.113.0/24"}, headers={"X-CSRF-Token": csrf})
    check("add an allowlist entry that excludes this client -> 200", status == 200, f"got {status}")
    ping = {"jsonrpc": "2.0", "id": 1, "method": "tools/list"}
    for label, hdrs in (("valid", mcp_auth), ("invalid", {"Authorization": "Bearer " + "0" * 64})):
        for route in ("/mcp", "/mcp/agent"):
            status, _, _ = anon.post_json(route, ping, headers=hdrs)
            check(f"{route} from outside the allowlist with a {label} token -> 403",
                  status == 403, f"got {status}")
    check("token last_used_at untouched by requests from outside the allowlist",
          token_last_used() == last_used_before,
          f"before={last_used_before!r} after={token_last_used()!r}")
    status, _, _ = admin.delete(
        "/api/admin/mcp-remote-config/allowed-cidrs?cidr=203.0.113.0/24",
        headers={"X-CSRF-Token": csrf})
    check("remove the allowlist entry -> 200", status == 200, f"got {status}")
    status, _, body = anon.post_json(
        "/mcp", {"jsonrpc": "2.0", "id": 1, "method": "initialize"}, headers=mcp_auth)
    server_info = (json.loads(body or "{}").get("result") or {}).get("serverInfo") or {}
    check("remote MCP initialize: serverInfo is name + bare version",
          status == 200 and server_info.get("name") == "personal-wiki"
          and server_info.get("version") == expected_version,
          f"status={status} serverInfo={server_info!r} expected version={expected_version!r}")

    mcp_bin = os.path.join(os.path.dirname(server_bin), "wiki-mcp")
    stdio = subprocess.run(
        [mcp_bin], cwd=sandbox, capture_output=True, text=True, timeout=20,
        input=json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize"}) + "\n")
    first_line = stdio.stdout.splitlines()[0] if stdio.stdout else "{}"
    stdio_info = (json.loads(first_line).get("result") or {}).get("serverInfo") or {}
    check("stdio wiki-mcp initialize: serverInfo is name + bare version",
          stdio_info.get("name") == "personal-wiki"
          and stdio_info.get("version") == expected_version,
          f"serverInfo={stdio_info!r} expected version={expected_version!r} "
          f"stderr={stdio.stderr[-300:]!r}")

    run_modern_mcp_checks(anon, mcp_auth, mcp_bin, sandbox, expected_version)
    run_skill_write_guard_checks(admin, anon, csrf, mcp_auth, mcp_bin, sandbox, vault)
    run_mcp_skills_checks(admin, anon, csrf, mcp_auth, mcp_bin, sandbox, vault)

    status, _, body = admin.post_json(
        "/mcp",
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
         "params": {"name": "attach_file",
                    "arguments": {"path": "notes/private.md",
                                  "source_path": "/etc/passwd"}}},
        headers=mcp_auth)
    rpc = json.loads(body) if body else {}
    hidden_dir = os.path.join(vault, ".hidden")
    os.makedirs(hidden_dir, exist_ok=True)
    with open(os.path.join(hidden_dir, "secret.md"), "w") as f:
        f.write("---\ntitle: Hidden\nvisibility: public\n---\nhiddenprobe\n")
    hidden_status, _, hidden_body = admin.post_json(
        "/mcp",
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
         "params": {"name": "get_document",
                    "arguments": {"id_or_path": ".hidden/secret.md"}}},
        headers=mcp_auth)
    hidden_rpc = json.loads(hidden_body) if hidden_body else {}
    check("remote get_document refuses a dot-directory path",
          hidden_status == 200 and hidden_rpc.get("result", {}).get("isError") is True
          and b"hiddenprobe" not in hidden_body,
          f"status={hidden_status} body={hidden_body[:240]!r}")
    for route in ("/api/documents/.hidden/secret.md", "/api/documents/.hidden/secret.md/raw",
                  "/api/attachments/.hidden/secret.md", "/api/graph?around=.hidden/secret.md"):
        hidden_status, _, _ = admin.get(route)
        check(f"admin GET {route} -> 404", hidden_status == 404, f"got {hidden_status}")

    check("remote attach_file source_path is rejected (host LFI)",
          status == 200 and rpc.get("result", {}).get("isError") is True
          and "source_path" in rpc.get("result", {}).get("content", [{}])[0].get("text", ""),
          f"status={status} body={body[:240]!r}")

    status, _, body = admin.post_json(
        "/mcp",
        {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
         "params": {"name": "attach_file_begin",
                    "arguments": {"path": "notes/private.md",
                                  "filename": "mcp-big.bin"}}},
        headers=mcp_auth)
    rpc = json.loads(body) if body else {}
    begin_text = rpc.get("result", {}).get("content", [{}])[0].get("text", "")
    upload_id_match = re.search(r"upload_id:\s*([0-9a-fA-F-]{36})", begin_text)
    check("attach_file_begin returns an upload_id",
          status == 200 and rpc.get("result", {}).get("isError") is not True
          and upload_id_match is not None,
          f"status={status} text={begin_text!r}")
    upload_id = upload_id_match.group(1) if upload_id_match else ""

    status, _, body = anon.request(
        "PUT", f"/mcp/uploads/{upload_id}",
        body=b"hello-mcp-upload",
        headers={"Content-Type": "application/octet-stream"})
    check("PUT raw bytes to capability URL (no Bearer) -> 201",
          status == 201, f"got {status} body={body[:200]!r}")
    uploaded = json.loads(body) if status == 201 and body else {}
    check("upload JSON has path and markdownLink",
          "path" in uploaded and "markdownLink" in uploaded, f"got {uploaded}")

    status, _, body = admin.get("/api/documents/notes/private.md")
    doc = json.loads(body) if body else {}
    check("owning document body contains the uploaded markdown link",
          status == 200 and uploaded.get("markdownLink", "MISSING") in doc.get("body", ""),
          f"status={status} body={doc.get('body', '')!r}")
    status, _, _ = anon.get(f"/assets/{uploaded.get('path', '')}")
    check("anon cannot fetch MCP-uploaded attachment of a private doc",
          status == 404, f"got {status}")

    status, _, _ = anon.request(
        "PUT", f"/mcp/uploads/{upload_id}",
        body=b"replay",
        headers={"Content-Type": "application/octet-stream"})
    check("replay of a consumed upload_id -> 404", status == 404, f"got {status}")
    status, _, _ = anon.request(
        "PUT", "/mcp/uploads/00000000-0000-4000-8000-000000000000",
        body=b"nope",
        headers={"Content-Type": "application/octet-stream"})
    check("PUT unknown upload id while enabled -> 404", status == 404, f"got {status}")

    # --- Remote agent endpoint (POST /mcp/agent): per-token opt-in, -------
    #     gated on top of (not instead of) a valid write-enabled token.
    #     The test server runs with [llm].provider = "none", so a fully
    #     authorized call still can't actually run the LLM -- that's the
    #     503 branch, which is exactly as far as this test can verify
    #     without a real API key, and it's enough to prove the whole
    #     auth/gating chain (enabled -> rate limit -> IP
    #     allowlist -> token -> LLM-configured -> writeEnabled -> agentAccess) ran
    #     to completion rather than rejecting earlier for the wrong reason.
    status, _, body = admin.post_json(
        "/mcp/agent", {"messages": [{"role": "user", "content": "hi"}]}, headers=mcp_auth)
    check("agent call with agentAccess off -> 403",
          status == 403, f"got {status} body={body[:200]!r}")
    status, _, _ = admin.post_json(
        "/api/admin/mcp-remote-config/tokens/agent-access",
        {"id": mcp_token_id, "enabled": True}, headers={"X-CSRF-Token": csrf})
    check("enable agent access for this token -> 200", status == 200, f"got {status}")
    status, _, body = admin.post_json(
        "/mcp/agent", {"messages": [{"role": "user", "content": "hi"}]}, headers=mcp_auth)
    check("agent call with agentAccess on, no LLM configured -> 503",
          status == 503, f"got {status} body={body[:200]!r}")
    status, _, _ = admin.post_json(
        "/api/admin/mcp-remote-config/tokens/agent-access",
        {"id": mcp_token_id, "enabled": False}, headers={"X-CSRF-Token": csrf})
    check("disable agent access again -> 200", status == 200, f"got {status}")
    status, _, _ = admin.post_json(
        "/mcp/agent", {"messages": [{"role": "user", "content": "hi"}]}, headers=mcp_auth)
    check("agent call with agentAccess off again -> 403", status == 403, f"got {status}")

    # --- Multi-token independence: a second token works on its own, and ---
    #     deleting the FIRST one doesn't touch it. The deliberately-wrong-
    #     token check below trips the /mcp rate limiter's 1s backoff for
    #     this IP (by design -- see RateLimiter.h), so it goes LAST here,
    #     before unrelated sections that don't touch /mcp again give that
    #     backoff time to expire on its own.
    status, _, body = admin.post_json(
        "/api/admin/mcp-remote-config/tokens", {"label": "e2e-secondary"},
        headers={"X-CSRF-Token": csrf})
    check("create a second remote MCP token -> 200", status == 200, f"got {status}")
    second = json.loads(body)
    second_token, second_id = second.get("token"), second.get("id")
    status, _, _ = admin.post_json(
        "/mcp", {"jsonrpc": "2.0", "id": 90, "method": "tools/list"},
        headers={"Authorization": f"Bearer {second_token}"})
    check("second token independently authenticates -> 200", status == 200, f"got {status}")
    status, _, _ = admin.delete(
        f"/api/admin/mcp-remote-config/tokens?id={second_id}",
        headers={"X-CSRF-Token": csrf})
    check("delete the second token -> 200", status == 200, f"got {status}")
    status, _, _ = admin.post_json(
        "/mcp", {"jsonrpc": "2.0", "id": 91, "method": "tools/list"},
        headers={"Authorization": f"Bearer {mcp_token}"})
    check("the OTHER (primary) token still authenticates after the second was deleted -> 200",
          status == 200, f"got {status}")
    status, _, _ = admin.post_json(
        "/mcp", {"jsonrpc": "2.0", "id": 92, "method": "tools/list"},
        headers={"Authorization": f"Bearer {second_token}"})
    check("the deleted token no longer authenticates -> 401", status == 401, f"got {status}")

    status, _, _ = admin.put_json(
        "/api/admin/mcp-remote-config",
        {"enabled": False, "writeEnabled": False},
        headers={"X-CSRF-Token": csrf})
    check("disable remote MCP after upload checks -> 200", status == 200, f"got {status}")

    run_hardening_checks(sandbox, vault, anon, admin, csrf, attach_path)

    # --- 8. Update, soft-delete -------------------------------------------
    status, _, _ = admin.put_json(
        "/api/documents/notes/public.md",
        {"title": "Public Doc Updated", "tags": ["e2e"], "visibility": "public",
         "type": "note", "body": "updated body"},
        headers={"X-CSRF-Token": csrf})
    check("update doc -> 200", status == 200, f"got {status}")
    status, _, _ = admin.delete("/api/documents/notes/public.md", headers={"X-CSRF-Token": csrf})
    check("soft delete -> 200", status == 200, f"got {status}")
    check("file actually moved to .trash/",
          os.path.exists(os.path.join(vault, ".trash", "notes", "public.md")))
    status, _, _ = admin.get("/api/documents/notes/public.md")
    check("deleted doc gone from the JSON API", status == 404, f"got {status}")
    # Soft delete keeps the file and its `visibility: public` front matter
    # under .trash/ -- that copy must not be addressable as a document.
    for who, client in (("anon", anon), ("admin", admin)):
        for suffix in ("", "/raw"):
            status, _, _ = client.get("/api/documents/.trash/notes/public.md" + suffix)
            check(f"{who} GET /api/documents/.trash/...{suffix} -> 404",
                  status == 404, f"got {status}")
    status, _, body = admin.get_json("/api/admin/trash")
    check("trashed doc still listed in the admin trash view",
          status == 200 and any(i["path"] == "notes/public.md" for i in body["items"]),
          f"status={status} body={body!r}")

    # --- 9. VaultWatcher: external filesystem change picked up live ----
    live_dir = os.path.join(vault, "external")
    os.makedirs(live_dir, exist_ok=True)
    with open(os.path.join(live_dir, "dropped.md"), "w") as f:
        f.write("---\ntitle: Dropped\nvisibility: public\n---\nwatcherprobe content\n")
    time.sleep(1.0)  # debounce (~300ms) + processing headroom
    status, _, body = anon.get_json("/api/search?q=watcherprobe")
    paths = [r["path"] for r in body["results"]]
    check("VaultWatcher indexed an externally-created file without --reindex",
          "external/dropped.md" in paths, f"paths={paths}")
    os.remove(os.path.join(live_dir, "dropped.md"))
    time.sleep(1.0)
    status, _, body = anon.get_json("/api/search?q=watcherprobe")
    paths = [r["path"] for r in body["results"]]
    check("VaultWatcher swept the externally-deleted file from the index",
          "external/dropped.md" not in paths, f"paths={paths}")

    # --- 10. Change admin password ----------------------------------------
    # Deliberately runs BEFORE the rate-limiter check below: RateLimiter is
    # keyed by IP, not by session/cookie (see RateLimiter.h), so the
    # deliberate run of failed logins in check 11 would otherwise still
    # have this IP locked out when the plain-login assertions here run.
    # Also deliberately near the end: this actually rotates the real admin
    # credential, so nothing above this point may depend on
    # "SuperSecret123" still being valid afterwards.
    other = Client(HOST, PORT)
    status, _, _ = other.post_json("/api/login", {"username": "admin", "password": "SuperSecret123"})
    check("second session login (pre password-change) -> 200", status == 200, f"got {status}")

    status, _, _ = anon.post_json(
        "/api/account/password", {"currentPassword": "SuperSecret123", "newPassword": "NewSecret456"})
    check("anon password change -> 401", status == 401, f"got {status}")

    status, _, _ = admin.post_json(
        "/api/account/password", {"currentPassword": "SuperSecret123", "newPassword": "NewSecret456"})
    check("password change without csrf header -> 403", status == 403, f"got {status}")

    status, _, _ = admin.post_json(
        "/api/account/password", {"currentPassword": "WRONG", "newPassword": "NewSecret456"},
        headers={"X-CSRF-Token": csrf})
    check("password change with wrong current password -> 401", status == 401, f"got {status}")
    status, _, _ = admin.post_json(
        "/api/account/password", {"currentPassword": "SuperSecret123", "newPassword": "Short12345!"},
        headers={"X-CSRF-Token": csrf})
    check("password change to an 11-character password -> 400", status == 400, f"got {status}")
    status, _, _ = admin.post_json(
        "/api/account/password",
        {"currentPassword": "SuperSecret123", "newPassword": "x" * 1025},
        headers={"X-CSRF-Token": csrf})
    check("password change to a 1025-byte password -> 400", status == 400, f"got {status}")
    status, _, _ = admin.post_json(
        "/api/account/password",
        {"currentPassword": "x" * 1025, "newPassword": "NewSecret456"},
        headers={"X-CSRF-Token": csrf})
    check("password change with a 1025-byte current password -> 400",
          status == 400, f"got {status}")
    status, _, _ = Client(HOST, PORT).post_json(
        "/api/login", {"username": "admin", "password": "x" * 1025})
    check("login with a 1025-byte password -> 400", status == 400, f"got {status}")

    # Request bodies over the 100 MiB cap are refused before any handler
    # runs, for an anonymous caller too. Drogon may answer 413 or just
    # drop the connection once it has seen Content-Length.
    oversized = b'{"username":"admin","password":"' + b"x" * (101 * 1024 * 1024) + b'"}'
    conn = http.client.HTTPConnection(HOST, PORT, timeout=120)
    try:
        conn.request("POST", "/api/login", body=oversized,
                     headers={"Content-Type": "application/json"})
        status = conn.getresponse().status
        outcome = f"status {status}"
        refused = status == 413
    except (ConnectionResetError, BrokenPipeError, ConnectionAbortedError,
            http.client.RemoteDisconnected) as e:
        outcome = f"connection closed by server: {e!r}"
        refused = True
    finally:
        conn.close()
    check(f"anonymous 101 MiB request body is refused ({outcome})", refused, outcome)
    status, _, _ = anon.get("/healthz")
    check("server still healthy after the oversized request", status == 200, f"got {status}")

    status, _, _ = admin.post_json(
        "/api/account/password", {"currentPassword": "SuperSecret123", "newPassword": "NewSecret456"},
        headers={"X-CSRF-Token": csrf})
    check("password change with correct current password -> 200", status == 200, f"got {status}")

    status, _, body = admin.get_json("/api/session")
    check("caller's OWN session survives its own password change",
          body == {"authenticated": True, "agentEnabled": False}, f"got {body}")

    status, _, body = other.get_json("/api/session")
    check("every OTHER session was invalidated by the password change",
          body == {"authenticated": False, "agentEnabled": False}, f"got {body}")

    # Success before failure, deliberately: a failed /api/login attempt
    # trips RateLimiter.recordFailure() (1s+ backoff on this IP, see
    # RateLimiter.cpp), and a follow-up request landing inside that
    # backoff window would get 429 instead of the 200 this is checking
    # for. A successful attempt calls recordSuccess() (clears the entry),
    # so doing the success check first keeps the failure check after it
    # from needing its own backoff-clearing delay.
    fresh = Client(HOST, PORT)
    status, _, _ = fresh.post_json("/api/login", {"username": "admin", "password": "NewSecret456"})
    check("new password accepted after change", status == 200, f"got {status}")
    status, _, _ = fresh.post_json("/api/login", {"username": "admin", "password": "SuperSecret123"})
    check("old password rejected after change", status == 401, f"got {status}")

    # --- 10b. Login is rate limited per proxy-supplied client IP, and the
    # cookies' Secure flag follows X-Forwarded-Proto behind a proxy --------
    def raw_login(password, extra_headers):
        conn = http.client.HTTPConnection(HOST, PORT, timeout=5)
        try:
            headers = {"Content-Type": "application/json"}
            headers.update(extra_headers)
            conn.request("POST", "/api/login",
                         body=json.dumps({"username": "admin", "password": password}),
                         headers=headers)
            resp = conn.getresponse()
            resp.read()
            cookies = [v for k, v in resp.getheaders() if k.lower() == "set-cookie"]
            return resp.status, cookies
        finally:
            conn.close()

    attacker = {"X-Real-IP": "203.0.113.7"}
    legit = {"X-Real-IP": "203.0.113.8"}
    attacker_statuses = [raw_login("WRONG", attacker)[0] for _ in range(6)]
    check("login limiter blocks the attacker's IP (429 seen)",
          429 in attacker_statuses, f"statuses={attacker_statuses}")
    status, _ = raw_login("NewSecret456", attacker)
    check("blocked IP stays blocked even with the right password -> 429",
          status == 429, f"got {status}")
    status, cookies = raw_login("NewSecret456", legit)
    check("a different client IP is not locked out by the attacker -> 200",
          status == 200, f"got {status}")

    https_headers = {"X-Real-IP": "203.0.113.9", "X-Forwarded-Proto": "https"}
    status, cookies = raw_login("NewSecret456", https_headers)
    check("login behind a TLS-terminating proxy sets both cookies",
          status == 200 and len(cookies) == 2, f"status={status} cookies={cookies}")
    check("cookies are Secure when X-Forwarded-Proto is https",
          all("; Secure" in c for c in cookies), f"cookies={cookies}")

    http_headers = {"X-Real-IP": "203.0.113.10", "X-Forwarded-Proto": "http"}
    status, cookies = raw_login("NewSecret456", http_headers)
    check("cookies are not Secure when X-Forwarded-Proto is http",
          status == 200 and cookies and not any("; Secure" in c for c in cookies),
          f"status={status} cookies={cookies}")

    # --- 11. Rate limiting on repeated failed logins ---------------------
    rl = Client(HOST, PORT)
    statuses = []
    for _ in range(6):
        s, _, _ = rl.post_json("/api/login", {"username": "admin", "password": "WRONG"})
        statuses.append(s)
    check("rate limiter engages after repeated failed logins (429 seen)",
          429 in statuses, f"statuses={statuses}")


if __name__ == "__main__":
    sys.exit(main())
