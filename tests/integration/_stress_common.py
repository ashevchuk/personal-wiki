"""
Shared boot/HTTP-client plumbing for the stress-test scripts
(stress_concurrency.py, stress_resources.py).

Deliberately NOT imported by security_e2e.py — that script is the
release-gating correctness check (see its own module docstring / the M2
postmortem in docs/architecture.md) and stays fully self-contained on
purpose, so nothing added here can ever destabilize it. Stress tests are a
different category: they probe timing/concurrency/resource behavior, not
pass/fail correctness, so they're wired into ctest under a "stress" LABEL
(see tests/CMakeLists.txt) — run explicitly (`ctest -L stress`) or on a
schedule, not on every plain `ctest` invocation, since they're slower and
some of their findings are informational rather than hard failures.

Each script gets its OWN dynamically-picked port (see free_port()) rather
than a shared hardcoded one, so two stress scripts (or a stress script and
security_e2e.py) can safely run concurrently under a parallel `ctest -j`.
"""
import http.client
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.parse


def free_port():
    """Binds to port 0 to let the OS pick a free ephemeral port, then
    releases it immediately. Inherently racy (something else could grab it
    before the server binds) but that race is exactly as real as what
    security_e2e.py accepts with its own fixed port, and running each
    script's own port through this avoids the WORSE, guaranteed collision
    of reusing that same fixed 8199 across scripts run in parallel."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Client:
    """Minimal HTTP client (stdlib only), cookie-tracking — same shape as
    security_e2e.py's, duplicated rather than imported so each script's
    correctness never depends on the other's internals changing."""

    def __init__(self, host, port, timeout=10):
        self.host = host
        self.port = port
        self.timeout = timeout
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
        conn = http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
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

    def delete(self, path, headers=None):
        return self.request("DELETE", path, headers=headers)

    def upload(self, path, filename, content, headers=None):
        boundary = "----wikiStressBoundary"
        body = (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="file"; filename="{filename}"\r\n'
            f"Content-Type: application/octet-stream\r\n\r\n"
        ).encode() + content + f"\r\n--{boundary}--\r\n".encode()
        h = dict(headers or {})
        h["Content-Type"] = f"multipart/form-data; boundary={boundary}"
        return self.request("POST", path, body=body, headers=h)


def wait_for_healthz(host, port, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            c = Client(host, port, timeout=2)
            status, _, _ = c.get("/healthz")
            if status == 200:
                return True
        except (ConnectionRefusedError, OSError):
            pass
        time.sleep(0.2)
    return False


class Server:
    """Boots a real wiki-server against a fresh sandbox vault, same shape
    as security_e2e.py's main(), factored out so both stress scripts share
    it verbatim. Use as a context manager."""

    def __init__(self, server_bin, threads=4, admin_password="SuperSecret123"):
        self.server_bin = os.path.abspath(server_bin)
        self.threads = threads
        self.admin_password = admin_password
        self.host = "127.0.0.1"
        self.port = free_port()
        self.sandbox = tempfile.mkdtemp(prefix="wiki-stress-")
        self.vault = os.path.join(self.sandbox, "vault")
        self.proc = None

    def __enter__(self):
        os.makedirs(self.vault, exist_ok=True)
        with open(os.path.join(self.sandbox, "config.toml"), "w") as f:
            f.write(f"""
[server]
listen_addr = "{self.host}"
port = {self.port}
threads = {self.threads}
[vault]
path = "{self.vault}"
[index]
db_path = "{self.sandbox}/index.db"
[mcp]
scope = "admin"
[log]
level = "warn"
""")
        # static/shell.html + JS/CSS are read off disk relative to CWD
        # (PageRoutes.cpp) — same reasoning as security_e2e.py's copytree.
        project_root = os.path.dirname(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        shutil.copytree(os.path.join(project_root, "static"),
                         os.path.join(self.sandbox, "static"))

        admin_proc = subprocess.run(
            [self.server_bin, "--create-admin"], cwd=self.sandbox,
            input=f"admin\n{self.admin_password}\n{self.admin_password}\n",
            text=True, capture_output=True,
        )
        if admin_proc.returncode != 0:
            shutil.rmtree(self.sandbox, ignore_errors=True)
            raise RuntimeError(f"--create-admin failed: {admin_proc.stdout} {admin_proc.stderr}")

        self.proc = subprocess.Popen(
            [self.server_bin], cwd=self.sandbox,
            stdout=open(os.path.join(self.sandbox, "server.log"), "w"),
            stderr=subprocess.STDOUT,
        )
        if not wait_for_healthz(self.host, self.port):
            self._kill()
            self._dump_log()
            shutil.rmtree(self.sandbox, ignore_errors=True)
            raise RuntimeError("server never became healthy")
        return self

    def client(self):
        return Client(self.host, self.port)

    def is_alive(self):
        """True iff the server process is still running AND still answers
        /healthz — used after a stress burst to confirm the process didn't
        crash or wedge under load."""
        if self.proc.poll() is not None:
            return False
        try:
            status, _, _ = self.client().get("/healthz")
            return status == 200
        except OSError:
            return False

    def rss_kb(self):
        """Current resident memory of the server process, from /proc — used
        to sanity-check that a burst of large uploads doesn't balloon
        memory unboundedly. Linux-only (matches this project's deployment
        target); returns None if unavailable."""
        try:
            with open(f"/proc/{self.proc.pid}/status") as f:
                for line in f:
                    if line.startswith("VmRSS:"):
                        return int(line.split()[1])
        except (OSError, ValueError, IndexError):
            return None
        return None

    def _kill(self):
        if self.proc is None:
            return
        self.proc.send_signal(signal.SIGTERM)
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def _dump_log(self):
        log_path = os.path.join(self.sandbox, "server.log")
        if os.path.exists(log_path):
            print("\n--- server.log tail ---", file=sys.stderr)
            with open(log_path) as f:
                print("".join(f.readlines()[-40:]), file=sys.stderr)

    def __exit__(self, exc_type, exc, tb):
        if exc_type is not None:
            self._dump_log()
        self._kill()
        shutil.rmtree(self.sandbox, ignore_errors=True)
        return False


def login_admin(server, password=None):
    c = server.client()
    status, _, _ = c.post_json("/api/login", {
        "username": "admin", "password": password or server.admin_password})
    if status != 200:
        raise RuntimeError(f"admin login failed: {status}")
    csrf = c.cookies.get("wiki_csrf_token")
    return c, csrf
