#!/usr/bin/env python3
"""
End-to-end security/correctness checks against a real wiki-server process:
auth, CSRF, path traversal, session fixation, visibility gating (search/
nav/attachments), rate limiting, admin password change (wrong-current-
password rejection, other-session invalidation), and VaultWatcher pickup
of external filesystem changes.

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

        run_checks(sandbox, vault)

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


def run_checks(sandbox, vault):
    anon = Client(HOST, PORT)

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
    status, _, _ = anon.post_json("/api/documents", {"path": "x.md", "title": "x", "body": "y"})
    check("anon create -> 401", status == 401, f"got {status}")
    status, _, _ = anon.put_json("/api/documents/x.md", {"title": "x", "body": "y"})
    check("anon update -> 401", status == 401, f"got {status}")
    status, _, _ = anon.delete("/api/documents/x.md")
    check("anon delete -> 401", status == 401, f"got {status}")
    status, _, _ = anon.post_json("/api/documents/move", {"oldPath": "x.md", "newPath": "y.md"})
    check("anon document move -> 401", status == 401, f"got {status}")
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
    status, _, _ = anon.post_json("/api/admin/mcp-remote-config/regenerate-token", {})
    check("anon regenerate-token -> 401", status == 401, f"got {status}")
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
    check("anon session check -> authenticated:false", body == {"authenticated": False}, f"got {body}")
    status, _, _ = admin.post_json("/api/login", {"username": "admin", "password": "SuperSecret123"})
    check("admin login -> 200", status == 200, f"got {status}")
    csrf = admin.cookies.get("wiki_csrf_token")
    check("csrf cookie set on login", csrf is not None)
    status, _, body = admin.get_json("/api/session")
    check("admin session check -> authenticated:true", body == {"authenticated": True}, f"got {body}")

    # --- 4. CSRF enforcement ---------------------------------------------
    status, _, _ = admin.put_json("/api/documents/nope.md", {"title": "x", "body": "y"})
    check("mutating request without csrf header -> 403", status == 403, f"got {status}")
    status, _, _ = admin.post_json("/api/documents/move", {"oldPath": "x.md", "newPath": "y.md"})
    check("document move without csrf header -> 403", status == 403, f"got {status}")

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
    # This route sat behind a general "^/api/documents/(.*)$" handler
    # registered earlier in DocumentRoutes.cpp -- Drogon matches regex
    # handlers in registration order, and the general handler's own
    # "(.*)" greedily swallowed a trailing "/raw" as part of its own
    # docPath, so this route was 100% unreachable until the ordering was
    # fixed. Real bug, not hypothetical -- caught building the document
    # view page's "Download" button, this route's first real caller.
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
        "/api/admin/mcp-remote-config/regenerate-token", {},
        headers={"X-CSRF-Token": csrf})
    check("regenerate remote MCP token -> 200", status == 200, f"got {status}")
    mcp_token = json.loads(body).get("token")
    check("regenerate-token returns a raw token once",
          isinstance(mcp_token, str) and len(mcp_token) >= 32, f"got {mcp_token!r}")
    status, _, _ = admin.put_json(
        "/api/admin/mcp-remote-config",
        {"enabled": True, "writeEnabled": True},
        headers={"X-CSRF-Token": csrf})
    check("enable remote MCP writes -> 200", status == 200, f"got {status}")

    mcp_auth = {"Authorization": f"Bearer {mcp_token}"}
    status, _, body = admin.post_json(
        "/mcp",
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
         "params": {"name": "attach_file",
                    "arguments": {"path": "notes/private.md",
                                  "source_path": "/etc/passwd"}}},
        headers=mcp_auth)
    rpc = json.loads(body) if body else {}
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

    status, _, _ = admin.put_json(
        "/api/admin/mcp-remote-config",
        {"enabled": False, "writeEnabled": False},
        headers={"X-CSRF-Token": csrf})
    check("disable remote MCP after upload checks -> 200", status == 200, f"got {status}")

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
        "/api/account/password", {"currentPassword": "SuperSecret123", "newPassword": "NewSecret456"},
        headers={"X-CSRF-Token": csrf})
    check("password change with correct current password -> 200", status == 200, f"got {status}")

    status, _, body = admin.get_json("/api/session")
    check("caller's OWN session survives its own password change",
          body == {"authenticated": True}, f"got {body}")

    status, _, body = other.get_json("/api/session")
    check("every OTHER session was invalidated by the password change",
          body == {"authenticated": False}, f"got {body}")

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
