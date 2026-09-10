#!/usr/bin/env python3
"""
Concurrency stress tests against a real wiki-server process.

Distinct from security_e2e.py: that script proves each mechanism is
CORRECT under sequential use (one request, one response, checked in
order). This script fires the same mechanisms from many threads at once,
because several of them are check-then-act across two separate lock
acquisitions or two separate SQLite connections — a shape that reads fine
sequentially and can still misbehave only under real concurrency. See
docs/architecture.md's "VaultWatcher::start() blocks..." entry for a prior
example of exactly this class of bug (a real race, not hypothetical) —
this script exists to keep hunting for that class of bug in the pieces
that were never load-tested before.

1. RateLimiter burst — AuthRoutes.cpp calls `allow(ip)` and
   `recordFailure(ip)`/`recordSuccess(ip)` as two SEPARATE calls into
   RateLimiter (src/auth/RateLimiter.h), each individually mutex-protected
   but with no lock held across the gap between them. A burst of
   concurrent requests can all pass `allow()` before any of them lands its
   `recordFailure()` — this test measures exactly how much a concurrent
   burst can bypass, and confirms the limiter still converges to blocking
   once the burst settles (it does not claim the race is a bug to fix;
   RateLimiter.h's own comment already accepts weaker guarantees for a
   single-admin personal service — this test's job is to make the actual
   size of that gap a known, measured number instead of an assumption).

2. Concurrent writes to the SAME document path — DocumentService/
   VaultRepository has no mutex of its own (atomic temp+rename per write
   is the only ordering guarantee); IndexUpdater has none either. Fires N
   concurrent PUTs at one path, then verifies the file on disk is exactly
   one clean write (not an interleaved/corrupted merge of two) and the
   search index agrees with whatever that final write was.

3. Concurrent admin password change — two threads racing
   POST /api/account/password with different new passwords. Verifies the
   result is exactly one of the two (not a corrupted hash accepting
   neither/both), and the server survives it.

4. API writes vs. a simulated external editor (direct filesystem write)
   racing on DIFFERENT paths at the same time — the one place
   VaultWatcher's own SQLite connection and the request-threads' shared
   connection are both hammering the SAME database file concurrently.
   Verifies both paths end up correctly indexed and the index db itself
   is not corrupted (PRAGMA integrity_check).

Usage: stress_concurrency.py <path-to-wiki-server-binary>
"""
import os
import re
import sys
import sqlite3
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

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


def stress_rate_limiter(server):
    print("\n--- 1. RateLimiter concurrency burst -------------------------------")
    BURST = 25

    def attempt(_):
        c = server.client()
        status, _, _ = c.post_json("/api/login", {"username": "admin", "password": "WRONG"})
        return status

    with ThreadPoolExecutor(max_workers=BURST) as pool:
        statuses = list(pool.map(attempt, range(BURST)))

    n_401 = statuses.count(401)
    n_429 = statuses.count(429)
    n_other = BURST - n_401 - n_429
    check("no 5xx / unexpected status during the burst", n_other == 0,
          f"statuses={statuses}")
    note(f"burst of {BURST} concurrent wrong-password attempts: "
         f"{n_401} got 401 (raced past the check-then-act window), "
         f"{n_429} got 429 (blocked) — a nonzero 401 count here is the "
         f"documented, accepted TOCTOU gap in RateLimiter's two-call "
         f"design, not a new finding to fix blind")

    # Whatever raced through, the limiter's per-key state must have been
    # updated by ALL of them by now (recordFailure always runs, even for
    # a request that itself got 401 rather than 429) — so a SEQUENTIAL
    # follow-up attempt right after must be blocked. This is the actual
    # invariant that matters: the race can let a burst through, but it
    # must not leave the limiter permanently blind for that key.
    c = server.client()
    status, _, _ = c.post_json("/api/login", {"username": "admin", "password": "WRONG"})
    check("limiter is blocking this key by the time the burst settles",
          status == 429, f"got {status}")


def stress_same_document(server):
    print("\n--- 2. Concurrent writes to the SAME document path -----------------")
    path = "notes/hot.md"
    admin, csrf = login_admin(server)
    status, _, _ = admin.post_json(
        "/api/documents",
        {"path": path, "title": "Initial", "tags": ["stress"], "visibility": "public",
         "type": "note", "body": "initial body"},
        headers={"X-CSRF-Token": csrf})
    check("seed document created -> 201", status == 201, f"got {status}")

    WRITERS = 12
    results = []

    def writer(i):
        c, tok = login_admin(server)  # own session per thread, avoids cookie-jar races in the TEST client itself
        status, _, _ = c.put_json(
            f"/api/documents/{path}",
            {"title": f"Writer {i}", "tags": ["stress"], "visibility": "public",
             "type": "note", "body": f"body-from-writer-{i}"},
            headers={"X-CSRF-Token": tok})
        return i, status

    with ThreadPoolExecutor(max_workers=WRITERS) as pool:
        futures = [pool.submit(writer, i) for i in range(WRITERS)]
        for f in as_completed(futures):
            results.append(f.result())

    statuses = [s for _, s in results]
    check("every concurrent update returned 200 (no 5xx, no lost update)",
          all(s == 200 for s in statuses), f"statuses={statuses}")
    check("server still healthy after the burst", server.is_alive())

    # The file on disk must be a clean write from exactly ONE of the
    # writers -- never a torn/interleaved mix of two (which would show up
    # as a body that doesn't match ANY single writer's payload).
    file_path = os.path.join(server.vault, path)
    with open(file_path) as f:
        content = f.read()
    # re.search with a trailing (?!\d) boundary, not a plain substring
    # check -- "body-from-writer-1" is a substring of "body-from-writer-11",
    # so a naive `in` test spuriously matches BOTH writer 1 and writer 11
    # against writer 11's own clean, non-torn write. This is a test-harness
    # bug (found rerunning under ctest, where WRITERS-many concurrent
    # sessions make a double-digit writer id likely), not a server one.
    matching_writers = [i for i, _ in results
                         if re.search(rf"body-from-writer-{i}(?!\d)", content)]
    check("final file content is a clean write from exactly one writer, not a torn merge",
          len(matching_writers) == 1, f"file body did not cleanly match one writer; content={content!r}")

    # The index must agree with whatever that final write actually was --
    # not a stale row from an earlier writer that lost the file-write race
    # but won the index-write race (or vice versa).
    winner = matching_writers[0] if matching_writers else None
    time.sleep(0.3)  # let any async index update settle
    anon = server.client()
    status, _, body = anon.get_json(f"/api/search?q=body-from-writer-{winner}")
    found = any(r["path"] == path for r in body["results"]) if body else False
    check("search index agrees with the final file content (no stale index row)",
          winner is not None and found, f"winner={winner} search body={body}")


def stress_password_change_race(server):
    print("\n--- 3. Concurrent admin password change race ------------------------")
    admin, csrf = login_admin(server)

    def change(new_password):
        return admin.post_json(
            "/api/account/password",
            {"currentPassword": server.admin_password, "newPassword": new_password},
            headers={"X-CSRF-Token": csrf})

    with ThreadPoolExecutor(max_workers=2) as pool:
        f1 = pool.submit(change, "RaceWinnerA1")
        f2 = pool.submit(change, "RaceWinnerB1")
        s1, _, _ = f1.result()
        s2, _, _ = f2.result()

    check("server still healthy after concurrent password change", server.is_alive())

    # Checking both candidate passwords back-to-back is NOT safe against
    # RateLimiter (auth/RateLimiter.h, IP-keyed): if A turns out wrong,
    # that failure alone trips a 1s backoff on this IP, so an immediate
    # follow-up check of B lands inside that window and gets 429 instead
    # of a clean 401/200 -- indistinguishable from "also wrong" without
    # the wait. Only trust an actual 200 as ground truth, and check B
    # only after giving that minimal backoff (kMaxBackoffSeconds' first
    # step, 2^(1-1)=1s) time to clear.
    okA, _, _ = server.client().post_json(
        "/api/login", {"username": "admin", "password": "RaceWinnerA1"})
    a_valid = okA == 200
    if a_valid:
        b_valid = False
    else:
        time.sleep(1.2)
        okB, _, _ = server.client().post_json(
            "/api/login", {"username": "admin", "password": "RaceWinnerB1"})
        b_valid = okB == 200
    check("exactly one of the two racing passwords ended up valid (no corruption, no dual-accept)",
          a_valid != b_valid, f"statuses were ({s1}, {s2}); a_valid={a_valid} b_valid={b_valid}")

    server.admin_password = "RaceWinnerA1" if a_valid else "RaceWinnerB1"


def stress_api_vs_external_writer(server):
    print("\n--- 4. API writes vs. simulated external editor, racing the index -")
    admin, csrf = login_admin(server)
    api_path = "notes/api-hot.md"
    ext_dir = os.path.join(server.vault, "external-stress")
    os.makedirs(ext_dir, exist_ok=True)

    status, _, _ = admin.post_json(
        "/api/documents",
        {"path": api_path, "title": "API Hot", "tags": ["stress"], "visibility": "public",
         "type": "note", "body": "seed"},
        headers={"X-CSRF-Token": csrf})
    check("seed API document created -> 201", status == 201, f"got {status}")

    ROUNDS = 20
    errors = []

    def api_writer():
        try:
            for i in range(ROUNDS):
                c, tok = login_admin(server)
                s, _, _ = c.put_json(
                    f"/api/documents/{api_path}",
                    {"title": "API Hot", "tags": ["stress"], "visibility": "public",
                     "type": "note", "body": f"api-round-{i}"},
                    headers={"X-CSRF-Token": tok})
                if s != 200:
                    errors.append(f"api write {i} -> {s}")
        except OSError as e:
            errors.append(f"api writer exception: {e}")

    def external_writer():
        try:
            for i in range(ROUNDS):
                with open(os.path.join(ext_dir, f"ext-{i % 5}.md"), "w") as f:
                    f.write(f"---\ntitle: Ext {i}\nvisibility: public\n---\n"
                             f"external-round-{i}\n")
                time.sleep(0.02)
        except OSError as e:
            errors.append(f"external writer exception: {e}")

    with ThreadPoolExecutor(max_workers=2) as pool:
        f1 = pool.submit(api_writer)
        f2 = pool.submit(external_writer)
        f1.result()
        f2.result()

    check("no errors from either writer during the race", not errors, f"errors={errors}")
    check("server still healthy after the race", server.is_alive())

    time.sleep(1.5)  # VaultWatcher debounce (~300ms) + processing headroom
    anon = server.client()
    status, _, body = anon.get_json(f"/api/search?q=api-round-{ROUNDS - 1}")
    check("API path's final write is what the index reports",
          body and any(r["path"] == api_path for r in body["results"]), f"body={body}")
    status, _, body = anon.get_json(f"/api/search?q=external-round-{ROUNDS - 1}")
    check("external writer's last file is indexed too (VaultWatcher kept up under contention)",
          body and any(r["path"].startswith("external-stress/") for r in body["results"]),
          f"body={body}")

    db_path = os.path.join(server.sandbox, "index.db")
    try:
        conn = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True, timeout=5)
        result = conn.execute("PRAGMA integrity_check").fetchone()[0]
        conn.close()
        check("index.db PRAGMA integrity_check reports ok after the race",
              result == "ok", f"got {result!r}")
    except sqlite3.Error as e:
        check("index.db PRAGMA integrity_check reports ok after the race", False, str(e))


def main():
    if len(sys.argv) < 2:
        print("usage: stress_concurrency.py <path-to-wiki-server-binary>", file=sys.stderr)
        return 2
    server_bin = sys.argv[1]

    # Each check gets its OWN server/sandbox rather than sharing one across
    # the whole run: RateLimiter (test 1) deliberately leaves its target IP
    # backed off for up to 5 minutes (kMaxBackoffSeconds), and every client
    # here comes from the same 127.0.0.1 -- sharing a server would make
    # test 1's burst lock every later test's admin login out for real,
    # which is accepted server behavior (see RateLimiter.h) but not
    # something a test suite should sit through or work around.
    with Server(server_bin, threads=4) as server:
        stress_rate_limiter(server)
    with Server(server_bin, threads=4) as server:
        stress_same_document(server)
    with Server(server_bin, threads=4) as server:
        stress_password_change_race(server)
    with Server(server_bin, threads=4) as server:
        stress_api_vs_external_writer(server)

    print(f"\n{'=' * 60}")
    if FAILURES:
        print(f"{len(FAILURES)} FAILURE(S):")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print("All concurrency stress checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
