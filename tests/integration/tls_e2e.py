#!/usr/bin/env python3
"""
End-to-end checks for standalone TLS mode ([tls] enabled = true): a real
wiki-server process actually terminates TLS itself, serves an HSTS header
over that connection, and — the one check that actually proves the
security property this mode requires — auth::clientIp() never trusts
X-Real-IP/X-Forwarded-For while [tls].enabled is true (main.cpp derives
the trust flag it passes to clientIp() as `!cfg.tls.enabled`, not a
separate config knob — there's by definition no reverse proxy in front
in this mode, so nothing legitimate ever sends these headers and a
spoofed one must never reset the remote-MCP rate limiter's bucket).

Kept separate from security_e2e.py (plain HTTP, no TLS) rather than
folding into it: self-signed-cert generation and HTTPS-with-no-verification
machinery is a different concern from that script's plain-HTTP assertions,
and keeping them apart means neither script's setup has to account for
the other's.

Usage: tls_e2e.py <path-to-wiki-server-binary>
"""
import http.client
import json
import os
import shutil
import signal
import ssl
import subprocess
import sys
import tempfile
import time

PORT = 8198
HOST = "127.0.0.1"

FAILURES = []


def check(desc, cond, detail=""):
    if cond:
        print(f"OK   {desc}")
    else:
        print(f"FAIL {desc}" + (f" ({detail})" if detail else ""))
        FAILURES.append(desc)


def generate_self_signed_cert(cert_path, key_path):
    """Throwaway self-signed cert/key via the `openssl` CLI (already a
    build/runtime dependency of this project) — generated fresh per test
    run rather than a committed fixture, so there's nothing to expire
    years from now and silently break CI."""
    proc = subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", key_path, "-out", cert_path,
         "-days", "1", "-subj", "/CN=wiki-tls-e2e-test"],
        capture_output=True, text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"openssl cert generation failed: {proc.stderr}")
    if not (os.path.exists(cert_path) and os.path.getsize(cert_path) > 0):
        raise RuntimeError("openssl reported success but cert_path is missing/empty")
    if not (os.path.exists(key_path) and os.path.getsize(key_path) > 0):
        raise RuntimeError("openssl reported success but key_path is missing/empty")


def insecure_ssl_context():
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


class Client:
    """Same shape as security_e2e.py's Client (cookie-tracking stdlib HTTP
    client), but over TLS against a self-signed cert — verification is
    deliberately disabled: this is about the server's own TLS termination
    and the headers/behavior over that connection, not about certificate
    trust chains."""

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
        conn = http.client.HTTPSConnection(self.host, self.port, timeout=5,
                                            context=insecure_ssl_context())
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

    def post_json(self, path, obj, headers=None):
        return self.request("POST", path, json_body=obj, headers=headers)

    def put_json(self, path, obj, headers=None):
        return self.request("PUT", path, json_body=obj, headers=headers)


def wait_for_healthz(timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            status, _, _ = Client(HOST, PORT).get("/healthz")
            if status == 200:
                return True
        except (ConnectionRefusedError, OSError, ssl.SSLError):
            pass
        time.sleep(0.2)
    return False


def write_config(sandbox, vault, cert_path, key_path):
    with open(os.path.join(sandbox, "config.toml"), "w") as f:
        f.write(f"""
[server]
listen_addr = "{HOST}"
port = {PORT}
threads = 2
[tls]
enabled = true
cert_file = "{cert_path}"
key_file = "{key_path}"
[vault]
path = "{vault}"
[index]
db_path = "{sandbox}/index.db"
[mcp]
scope = "admin"
[log]
level = "warn"
""")


def start_server(server_bin, sandbox, vault, cert_path, key_path):
    """Creates the admin account (if not already present in this sandbox)
    and starts a fresh wiki-server process. Returns the Popen handle, or
    None if --create-admin failed."""
    write_config(sandbox, vault, cert_path, key_path)

    project_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    static_dst = os.path.join(sandbox, "static")
    if not os.path.isdir(static_dst):
        shutil.copytree(os.path.join(project_root, "static"), static_dst)

    if not os.path.exists(os.path.join(sandbox, "index.db")):
        admin_proc = subprocess.run(
            [server_bin, "--create-admin"], cwd=sandbox,
            input="admin\nSuperSecret123\nSuperSecret123\n",
            text=True, capture_output=True,
        )
        if admin_proc.returncode != 0:
            print("--create-admin failed:", admin_proc.stdout, admin_proc.stderr)
            return None

    return subprocess.Popen(
        [server_bin], cwd=sandbox,
        stdout=open(os.path.join(sandbox, "server.log"), "a"),
        stderr=subprocess.STDOUT,
    )


def stop_server(server):
    server.send_signal(signal.SIGTERM)
    try:
        server.wait(timeout=5)
    except subprocess.TimeoutExpired:
        server.kill()


def enable_remote_mcp(admin):
    """Logs an admin client in (fresh session each call -- cookies live on
    the Client instance) and turns on remote MCP with a throwaway token.
    Returns the bearer token."""
    status, _, _ = admin.post_json("/api/login", {"username": "admin", "password": "SuperSecret123"})
    if status != 200:
        raise RuntimeError(f"admin login failed: {status}")
    csrf = admin.cookies.get("wiki_csrf_token")
    if not csrf:
        raise RuntimeError("no csrf cookie after login")

    status, _, body = admin.post_json(
        "/api/admin/mcp-remote-config/regenerate-token", {},
        headers={"X-CSRF-Token": csrf})
    if status != 200:
        raise RuntimeError(f"regenerate-token failed: {status}")
    token = json.loads(body).get("token")
    if not token:
        raise RuntimeError(f"regenerate-token returned no token: {body!r}")

    status, _, _ = admin.put_json(
        "/api/admin/mcp-remote-config", {"enabled": True},
        headers={"X-CSRF-Token": csrf})
    if status != 200:
        raise RuntimeError(f"enabling remote MCP failed: {status}")
    return token


def spoofed_rate_limit_probe(n):
    """POSTs to /mcp with a wrong bearer token N times, each carrying a
    DIFFERENT spoofed X-Real-IP. In standalone TLS mode clientIp() always
    falls back to the raw TCP peer (there's by definition no reverse
    proxy to normalize this header) -- so every request must land in the
    SAME real-peer bucket and trip the limiter at the normal threshold,
    regardless of what X-Real-IP claims. /api/login deliberately never
    uses clientIp() at all (see ClientIp.h's own comment -- it's a
    hardcoded exception, always keyed on the real peer), so this probes
    /mcp's limiter instead, which does."""
    client = Client(HOST, PORT)
    statuses = []
    for i in range(n):
        status, _, _ = client.request(
            "POST", "/mcp",
            headers={
                "X-Real-IP": f"203.0.113.{i}",
                "Content-Type": "application/json",
                "Authorization": "Bearer definitely-wrong-token",
            },
            body=json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize"}).encode(),
        )
        statuses.append(status)
    return statuses


def run_spoofed_ip_check(server_bin):
    """Standalone TLS mode, remote MCP enabled, N requests each carrying
    a DIFFERENT spoofed X-Real-IP -- must still all land in the same
    real-peer rate-limiter bucket and trip it, proving clientIp() never
    trusted the header in this mode."""
    sandbox = tempfile.mkdtemp(prefix="wiki-tls-e2e-trust-")
    vault = os.path.join(sandbox, "vault")
    os.makedirs(vault, exist_ok=True)
    cert_path = os.path.join(sandbox, "fullchain.pem")
    key_path = os.path.join(sandbox, "privkey.pem")
    generate_self_signed_cert(cert_path, key_path)

    server = start_server(server_bin, sandbox, vault, cert_path, key_path)
    try:
        if server is None or not wait_for_healthz():
            FAILURES.append("server startup (spoofed-ip check)")
            return

        enable_remote_mcp(Client(HOST, PORT))
        statuses = spoofed_rate_limit_probe(8)
        check("standalone TLS: a spoofed X-Real-IP does NOT reset the "
              "rate limiter's bucket -- same real-peer bucket trips at "
              "the normal threshold despite 8 differently-spoofed attempts",
              429 in statuses, statuses)
    finally:
        if server is not None:
            stop_server(server)
        if FAILURES:
            log_path = os.path.join(sandbox, "server.log")
            if os.path.exists(log_path):
                print("\n--- server.log tail (spoofed-ip check) ---")
                with open(log_path) as f:
                    print("".join(f.readlines()[-30:]))
        shutil.rmtree(sandbox, ignore_errors=True)


def main():
    if len(sys.argv) < 2:
        print("usage: tls_e2e.py <path-to-wiki-server-binary>", file=sys.stderr)
        return 2
    server_bin = os.path.abspath(sys.argv[1])

    # --- 1. Basic standalone-TLS smoke test: real HTTPS, HSTS header. ---
    sandbox = tempfile.mkdtemp(prefix="wiki-tls-e2e-")
    vault = os.path.join(sandbox, "vault")
    os.makedirs(vault, exist_ok=True)
    cert_path = os.path.join(sandbox, "fullchain.pem")
    key_path = os.path.join(sandbox, "privkey.pem")
    generate_self_signed_cert(cert_path, key_path)

    server = start_server(server_bin, sandbox, vault, cert_path, key_path)
    try:
        if server is None or not wait_for_healthz():
            print("server never became healthy over TLS")
            FAILURES.append("server startup")
        else:
            status, headers, _ = Client(HOST, PORT).get("/healthz")
            check("GET /healthz over HTTPS (self-signed cert) -> 200", status == 200, status)
            check("Strict-Transport-Security present on a real TLS connection",
                  "strict-transport-security" in {k.lower() for k in headers})
    finally:
        if server is not None:
            stop_server(server)
        if FAILURES:
            print("\n--- server.log tail (failures occurred) ---")
            with open(os.path.join(sandbox, "server.log")) as f:
                print("".join(f.readlines()[-30:]))
        shutil.rmtree(sandbox, ignore_errors=True)

    # --- 2. The actual security property: clientIp() ignores a spoofed
    #        X-Real-IP in standalone TLS mode. ---
    run_spoofed_ip_check(server_bin)

    print(f"\n{'=' * 60}")
    if FAILURES:
        print(f"{len(FAILURES)} FAILURE(S):")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print("All TLS E2E checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
