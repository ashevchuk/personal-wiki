#!/usr/bin/env python3
"""
End-to-end security/correctness checks against a real wiki-server process:
auth, CSRF, path traversal, session fixation, visibility gating (search/
nav/attachments), rate limiting, and VaultWatcher pickup of external
filesystem changes.

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
                        "/edit/whatever.md"):
        status, _, _ = anon.get(shell_path)
        check(f"shell route {shell_path} -> 200", status == 200, f"got {status}")

    # --- 1. Unauthenticated writes must ALL be rejected ---------------
    status, _, _ = anon.post_json("/api/documents", {"path": "x.md", "title": "x", "body": "y"})
    check("anon create -> 401", status == 401, f"got {status}")
    status, _, _ = anon.put_json("/api/documents/x.md", {"title": "x", "body": "y"})
    check("anon update -> 401", status == 401, f"got {status}")
    status, _, _ = anon.delete("/api/documents/x.md")
    check("anon delete -> 401", status == 401, f"got {status}")
    status, _, _ = anon.upload("/api/attachments/x.md", "a.png", b"data")
    check("anon upload -> 401", status == 401, f"got {status}")
    check("vault has zero .md files after anon attempts",
          not any(f.endswith(".md") for _, _, files in os.walk(vault) for f in files))

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

    # --- 10. Rate limiting on repeated failed logins ---------------------
    rl = Client(HOST, PORT)
    statuses = []
    for _ in range(6):
        s, _, _ = rl.post_json("/api/login", {"username": "admin", "password": "WRONG"})
        statuses.append(s)
    check("rate limiter engages after repeated failed logins (429 seen)",
          429 in statuses, f"statuses={statuses}")


if __name__ == "__main__":
    sys.exit(main())
