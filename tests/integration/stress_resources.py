#!/usr/bin/env python3
"""
Resource-exhaustion stress tests against a real wiki-server process — the
"can a client degrade or crash the service without ever breaking auth/CSRF/
traversal checks" category, distinct from both security_e2e.py (logical
correctness) and stress_concurrency.py (race conditions).

1. Concurrent near-cap attachment uploads — AttachmentService caps a single
   attachment at 25 MiB (src/vault/AttachmentService.cpp) and Drogon itself
   is capped at 30 MiB (setClientMaxBodySize, src/main.cpp) with 5 MiB of
   headroom over that for the multipart envelope. This fires several
   near-cap uploads AT ONCE and checks the server's own memory doesn't
   balloon far past what that many legitimate near-cap files should cost,
   and that it stays responsive throughout.

2. Oversized single upload (over both caps) — must be rejected promptly,
   not accepted, and not left as a partial/oversized file on disk.

3. Heavy concurrent search load vs. /healthz responsiveness — a burst of
   expensive concurrent FTS5 queries must not starve the Drogon thread
   pool enough to make an unrelated, cheap request (/healthz) stall.

4. Pathological path input on document creation (very long path, many
   segments) — must be a clean rejection, not a hang or a crash; separately
   confirmes PathGuard's own containment holds under an oversized input,
   not just the short payloads unit-tested in PathGuardTest.cpp.

Usage: stress_resources.py <path-to-wiki-server-binary>
"""
import os
import sys
import time
import urllib.parse
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _stress_common import Server, login_admin  # noqa: E402

FAILURES = []


def check(desc, cond, detail=""):
    if cond:
        print(f"OK   {desc}")
    else:
        print(f"FAIL {desc}" + (f" ({detail})" if detail else ""))
        FAILURES.append(desc)


def note(msg):
    print(f"note {msg}")


def stress_concurrent_uploads(server):
    print("\n--- 1. Concurrent near-cap attachment uploads -----------------------")
    admin, csrf = login_admin(server)
    status, _, _ = admin.post_json(
        "/api/documents",
        {"path": "notes/uploads.md", "title": "Uploads", "tags": [], "visibility": "public",
         "type": "note", "body": "target for stress uploads"},
        headers={"X-CSRF-Token": csrf})
    check("seed document created -> 201", status == 201, f"got {status}")

    UPLOADERS = 6
    SIZE = 24 * 1024 * 1024  # just under the 25 MiB app-level cap
    payload = os.urandom(1024) * (SIZE // 1024)  # not all-zero, avoids sparse-file skew

    rss_before = server.rss_kb()

    def upload(i):
        c, tok = login_admin(server)
        start = time.monotonic()
        s, _, _ = c.upload(f"/api/attachments/notes/uploads.md", f"stress-{i}.bin", payload,
                            headers={"X-CSRF-Token": tok})
        return i, s, time.monotonic() - start

    with ThreadPoolExecutor(max_workers=UPLOADERS) as pool:
        results = list(pool.map(upload, range(UPLOADERS)))

    statuses = [s for _, s, _ in results]
    check("every near-cap concurrent upload accepted (201) -- none dropped under load",
          all(s == 201 for s in statuses), f"statuses={statuses}")
    check("server still healthy after the concurrent upload burst", server.is_alive())

    rss_after = server.rss_kb()
    if rss_before is not None and rss_after is not None:
        grew_mb = (rss_after - rss_before) / 1024
        note(f"server RSS: {rss_before / 1024:.1f} MiB -> {rss_after / 1024:.1f} MiB "
             f"(+{grew_mb:.1f} MiB) after {UPLOADERS}x{SIZE / (1024 * 1024):.0f} MiB concurrent uploads")
        # Generous bound: legitimate reasons to hold multiple bodies in
        # flight briefly, so this isn't "must be near zero" -- it's "must
        # not be holding many multiples of the actual payload size",
        # which is what an unbounded-buffering bug would look like.
        check("RSS growth stays within a sane multiple of the data actually uploaded",
              grew_mb < (UPLOADERS * SIZE / (1024 * 1024)) * 3,
              f"grew {grew_mb:.1f} MiB for {UPLOADERS * SIZE / (1024 * 1024):.0f} MiB of uploads")
    else:
        note("could not read /proc/<pid>/status -- skipping RSS bound check (non-Linux?)")


def stress_oversized_upload(server):
    print("\n--- 2. Single oversized upload (over both caps) ---------------------")
    admin, csrf = login_admin(server)
    status, _, _ = admin.post_json(
        "/api/documents",
        {"path": "notes/oversized.md", "title": "Oversized", "tags": [], "visibility": "public",
         "type": "note", "body": "target"},
        headers={"X-CSRF-Token": csrf})
    check("seed document created -> 201", status == 201, f"got {status}")

    SIZE = 50 * 1024 * 1024  # over both the 25 MiB app cap and 30 MiB Drogon cap
    payload = os.urandom(1024) * (SIZE // 1024)

    start = time.monotonic()
    try:
        status, _, _ = admin.upload("/api/attachments/notes/oversized.md", "toobig.bin", payload,
                                     headers={"X-CSRF-Token": csrf})
    except OSError as e:
        # A hard connection reset while sending is an ACCEPTABLE way for
        # Drogon to reject an over-cap body -- still "promptly rejected,
        # not accepted", just at the transport layer instead of HTTP.
        status = None
        note(f"connection-level rejection while sending oversized body: {e}")
    elapsed = time.monotonic() - start

    check("oversized upload was not accepted (no 201)", status != 201, f"got {status}")
    check("oversized upload was rejected promptly, not held open",
          elapsed < 15, f"took {elapsed:.1f}s")

    # Whatever happened at the HTTP layer, no oversized file may have
    # landed on disk -- that would mean the cap was enforced only after
    # already having paid the full cost of writing it.
    assets_dir = os.path.join(server.vault, "notes", "oversized.assets")
    leaked = []
    if os.path.isdir(assets_dir):
        for name in os.listdir(assets_dir):
            p = os.path.join(assets_dir, name)
            if os.path.getsize(p) > 30 * 1024 * 1024:
                leaked.append((name, os.path.getsize(p)))
    check("no oversized attachment file leaked onto disk", not leaked, f"leaked={leaked}")


def stress_search_vs_healthz(server):
    print("\n--- 3. Heavy concurrent search load vs. /healthz responsiveness -----")
    admin, csrf = login_admin(server)
    for i in range(15):
        words = " ".join(f"stressword{i}-{j}" for j in range(40))
        admin.post_json(
            "/api/documents",
            {"path": f"notes/search-load-{i}.md", "title": f"Search Load {i}", "tags": ["stress"],
             "visibility": "public", "type": "note", "body": words},
            headers={"X-CSRF-Token": csrf})

    # A query built to make FTS5 do real work: FtsSearch::buildMatchExpression
    # (src/index/FtsSearch.cpp) quotes every whitespace-split word as an
    # independent, implicitly-ANDed prefix term -- so a query with many
    # DISTINCT words (an "OR" spelled out as a literal word included) would
    # actually narrow the match to near-zero rows, the opposite of heavy.
    # A single broad PREFIX term that matches every seeded document instead
    # forces snippet() to run across the whole hit set -- genuinely more
    # work per query, and the concurrency (20 at once) is what stresses the
    # thread pool regardless of how heavy any one query is.
    heavy_query = "stressword"

    stop = {"flag": False}
    healthz_latencies = []

    def healthz_probe():
        c = server.client()
        while not stop["flag"]:
            start = time.monotonic()
            try:
                s, _, _ = c.get("/healthz")
                healthz_latencies.append((time.monotonic() - start, s))
            except OSError as e:
                healthz_latencies.append((time.monotonic() - start, str(e)))
            time.sleep(0.05)

    def search_hammer():
        c = server.client()
        try:
            c.get(f"/api/search?q={urllib.parse.quote(heavy_query)}")
        except OSError:
            pass

    with ThreadPoolExecutor(max_workers=21) as pool:
        probe_future = pool.submit(healthz_probe)
        search_futures = [pool.submit(search_hammer) for _ in range(20)]
        for f in search_futures:
            f.result()
        stop["flag"] = True
        probe_future.result()

    check("server still healthy after the search burst", server.is_alive())
    bad = [(lat, s) for lat, s in healthz_latencies if s != 200]
    check("every /healthz probe during the burst got 200 (no starvation-induced errors)",
          not bad, f"bad samples={bad[:5]}{'...' if len(bad) > 5 else ''}")
    if healthz_latencies:
        max_lat = max(lat for lat, _ in healthz_latencies)
        note(f"/healthz latency during concurrent search burst: max={max_lat:.2f}s "
             f"over {len(healthz_latencies)} probes")
        check("no /healthz probe stalled more than 5s (thread pool not fully starved)",
              max_lat < 5.0, f"max={max_lat:.2f}s")


def stress_pathological_path(server):
    print("\n--- 4. Pathological path input on document creation ------------------")
    admin, csrf = login_admin(server)

    # Genuinely rejection-worthy inputs: each must come back as a clean
    # 4xx, never a 500 (an uncaught/leaking exception) and never a 201
    # (silently accepting something that shouldn't exist).
    long_segment = "a" * 8000
    reject_cases = {
        # Over ext4/most Linux filesystems' 255-byte NAME_MAX for a single
        # path component -- PathGuard only checks traversal/absoluteness/
        # symlinks/NUL, not per-segment length, so this is expected to
        # reach the filesystem layer and fail THERE.
        "single 8000-char segment (over NAME_MAX)": f"notes/{long_segment}.md",
        "null-byte-ish payload": "notes/evil\x00.md",
        "many .. segments": "../" * 200 + "etc/passwd",
    }
    for label, path in reject_cases.items():
        start = time.monotonic()
        try:
            status, _, body = admin.post_json(
                "/api/documents",
                {"path": path, "title": "x", "tags": [], "visibility": "public",
                 "type": "note", "body": "y"},
                headers={"X-CSRF-Token": csrf})
        except OSError as e:
            status, body = None, None
            note(f"{label}: connection-level rejection ({e})")
        elapsed = time.monotonic() - start
        check(f"pathological path [{label}] rejected cleanly (not 201/5xx)",
              status not in (201, 500), f"got {status} body={body}")
        check(f"pathological path [{label}] handled promptly (no hang)",
              elapsed < 5, f"took {elapsed:.1f}s")
        if status == 500:
            note(f"[{label}] 500 body: {body}")

    # A LEGITIMATE extreme, not an attack: 500 levels of "dir/" segments
    # (~2000 bytes total, under Linux's 4096-byte PATH_MAX) is a lot, but
    # nothing here says it should be rejected -- this checks the server
    # handles a legitimate extreme cleanly (create AND read back), rather
    # than assuming "unusual" and "must-reject" are the same thing.
    deep_path = "/".join(["dir"] * 500) + "/deep.md"
    start = time.monotonic()
    status, _, _ = admin.post_json(
        "/api/documents",
        {"path": deep_path, "title": "Deep", "tags": [], "visibility": "public",
         "type": "note", "body": "deep content"},
        headers={"X-CSRF-Token": csrf})
    elapsed = time.monotonic() - start
    check("500-levels-deep (but PATH_MAX-legal) path handled promptly (no hang)",
          elapsed < 5, f"took {elapsed:.1f}s")
    if status == 201:
        status2, _, _ = admin.get(f"/api/documents/{deep_path}")
        check("legitimately deep path: created doc reads back cleanly (no half-created state)",
              status2 == 200, f"create -> 201, read back -> {status2}")
    else:
        check("legitimately deep path: rejection (if any) was clean, not a 500",
              status != 500, f"got {status}")

    check("server still healthy after pathological path inputs", server.is_alive())

    # The truly rejection-worthy cases (long segment, traversal) must never
    # have landed on disk -- the deep-but-legal path is deliberately
    # excluded from this scan since creating it there is correct behavior.
    leaked = []
    for root, _, files in os.walk(server.vault):
        rel_root = os.path.relpath(root, server.vault)
        if rel_root == "dir" or rel_root.startswith("dir" + os.sep):
            continue  # the legitimate 500-levels-deep tree created above
        for name in files:
            if len(name) > 200:
                leaked.append(os.path.join(root, name))
    check("no over-length path escaped onto disk", not leaked, f"leaked={leaked}")


def main():
    if len(sys.argv) < 2:
        print("usage: stress_resources.py <path-to-wiki-server-binary>", file=sys.stderr)
        return 2
    server_bin = sys.argv[1]

    with Server(server_bin, threads=4) as server:
        stress_concurrent_uploads(server)
        stress_oversized_upload(server)
        stress_search_vs_healthz(server)
        stress_pathological_path(server)

    print(f"\n{'=' * 60}")
    if FAILURES:
        print(f"{len(FAILURES)} FAILURE(S):")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print("All resource-exhaustion stress checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
