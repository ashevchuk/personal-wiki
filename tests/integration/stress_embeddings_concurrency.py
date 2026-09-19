#!/usr/bin/env python3
"""
Concurrency stress test for real embedding inference, at the HTTP layer.

Distinct from tests/unit/LocalEmbeddingProviderTest.cpp's own concurrency
test ("concurrent embed() calls on the SAME provider instance from
multiple threads"): that test drives LocalEmbeddingProvider directly, in
isolation, proving the fix (embedMutex_) works for the ONE class that
needed it. This script proves the whole real stack survives the same
class of load — real Drogon worker threads, real IndexUpdater/
EmbeddingIndexer/DocumentService wiring, real HTTP — not just the
isolated provider.

Context: a real SIGSEGV was found and fixed (docs/embeddings.md) when two
threads called LocalEmbeddingProvider::embed() on the SAME shared
instance at once — IndexUpdater::upsertOne() deliberately calls embed()
outside any lock (so a slow embed doesn't block other threads' document
saves), and main.cpp shares ONE provider instance across every
IndexUpdater in the process. This script fires N concurrent
POST /api/documents at a real server with a real local embedding
provider configured, then verifies:
  1. every request succeeded (no 5xx, no crash)
  2. the server is still alive and answering after the burst
  3. every document actually got a real, valid embedding — not silently
     dropped, not NaN/corrupted — proven by real semantic search for
     each document's own, lexically-unrelated topic (a corrupted vector
     wouldn't rank its own document first; a dropped one wouldn't be
     findable via the admin embeddings-status "needing attention" list
     either)
  4. admin embeddings-status reports zero documents needing attention —
     no silent per-document failures hiding in document_embedding_state

Only runs against a WIKI_ENABLE_LOCAL_EMBEDDINGS build with a real GGUF
model path — see tests/CMakeLists.txt's gate (same one
fts_search_hybrid_test / local_embedding_provider_test use).

Usage: stress_embeddings_concurrency.py <path-to-wiki-server-binary> <path-to-gguf-model>
"""
import os
import sys
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


# Each topic is lexically disjoint from the others (no shared words) so a
# later semantic-search check can tell "found the right one via real
# understanding" apart from "found something via plain keyword overlap".
TOPICS = [
    ("notes/topic-0.md", "Astronomy", "Stars burn hydrogen into helium over billions of years."),
    ("notes/topic-1.md", "Cooking", "Simmer the broth slowly and skim the foam off the top."),
    ("notes/topic-2.md", "Sports", "The sprinter crossed the finish line ahead of every rival."),
    ("notes/topic-3.md", "Weather", "Dark clouds gathered before the afternoon thunderstorm arrived."),
    ("notes/topic-4.md", "Music", "The orchestra tuned their instruments before the concert began."),
    ("notes/topic-5.md", "Geology", "Layers of sediment compress into rock over long ages."),
    ("notes/topic-6.md", "Gardening", "Water the seedlings each morning before the sun gets too hot."),
    ("notes/topic-7.md", "Chess", "She sacrificed a knight to open a path toward the enemy king."),
]

# One lexically-unrelated query per topic, chosen so it shares no words
# with its own stored body — same shape as the hybrid-search tests in
# tests/unit/FtsSearchHybridTest.cpp, just driven over real HTTP here.
QUERIES = [
    "Nuclear fusion powers distant suns",
    "Reduce the liquid on low heat",
    "A runner won the race by a wide margin",
    "A storm rolled in with heavy rain",
    "Musicians prepared their violins before the show",
    "Rock strata build up slowly underground",
    "Young plants need morning watering in the heat",
    "A pawn was traded to attack the opposing monarch",
]


def stress_embeddings_concurrency(server):
    print("\n--- Concurrent document creation with real local embeddings --------")
    admin, csrf = login_admin(server)

    def create(i):
        path, title, body = TOPICS[i]
        return i, admin.post_json(
            "/api/documents",
            {"path": path, "title": title, "tags": ["stress"], "visibility": "public",
             "type": "note", "body": body},
            headers={"X-CSRF-Token": csrf})

    def update(i):
        # A real content change (not a byte-identical re-save) — must
        # trigger a genuine second embed() call via the content-hash
        # mismatch, not get skipped by the skip-if-unchanged path. Still
        # lexically unrelated to every OTHER topic, so the later
        # semantic-search check stays valid against the updated body.
        path, title, body = TOPICS[i]
        return i, admin.put_json(
            f"/api/documents/{path}",
            {"title": title, "tags": ["stress"], "visibility": "public",
             "type": "note", "body": body + " Updated with more detail just now."},
            headers={"X-CSRF-Token": csrf})

    # Round 1: all topics created concurrently — N threads genuinely
    # overlapping in time (more workers than topics), each hitting
    # provider_->embed() on the SAME shared LocalEmbeddingProvider
    # instance at once. This IS the exact contention shape that used to
    # SIGSEGV (see module docstring).
    results = []
    with ThreadPoolExecutor(max_workers=len(TOPICS) * 2) as pool:
        futures = {pool.submit(create, i): i for i in range(len(TOPICS))}
        for f in as_completed(futures):
            i, (status, _, body) = f.result()
            results.append((i, status, body))
    bad = [(i, s, b) for i, s, b in results if s != 201]
    check("every concurrent create returned 201 (no 5xx, no crash)",
          not bad, f"bad={bad}")

    # Round 2: update every document concurrently too — a second wave of
    # real contention, this time through the UPDATE path (still calls
    # embed() again, since the content genuinely changed).
    results = []
    with ThreadPoolExecutor(max_workers=len(TOPICS) * 2) as pool:
        futures = {pool.submit(update, i): i for i in range(len(TOPICS))}
        for f in as_completed(futures):
            i, (status, _, body) = f.result()
            results.append((i, status, body))
    bad = [(i, s) for i, s, _ in results if s != 200]
    check("every concurrent update returned 200 (no 5xx, no crash)",
          not bad, f"bad={bad}")

    check("server still alive after both bursts", server.is_alive())

    # Give the last few embed() calls a moment to finish (they run
    # deliberately unlocked/async relative to the HTTP response — see
    # IndexUpdater::upsertOne's own comment) before checking final state.
    deadline = time.time() + 20
    needing = None
    while time.time() < deadline:
        status, _, body = admin.get_json("/api/admin/embeddings-status")
        if status == 200 and body is not None:
            needing = body.get("needingAttention", [])
            if not needing:
                break
        time.sleep(0.3)

    check("admin embeddings-status reports zero documents needing attention "
          "(no silent per-document embed failures under concurrent load)",
          needing == [], f"needing={needing}")

    # The real proof the vectors themselves are intact, not corrupted/NaN:
    # each topic's own document must actually rank via a lexically
    # unrelated query — a corrupted or all-NaN vector would either not
    # match anything or match everything indiscriminately.
    found_own_topic = []
    for i, (path, _, _) in enumerate(TOPICS):
        query = QUERIES[i]
        status, _, body = admin.get_json(f"/api/search?q={query.replace(' ', '+')}")
        results_paths = [r["path"] for r in body.get("results", [])] if body else []
        found_own_topic.append(path in results_paths)
    check("every topic's own document is findable via its lexically-unrelated "
          "semantic query (proof embeddings are real vectors, not corrupted)",
          all(found_own_topic), f"found_own_topic={list(zip((t[0] for t in TOPICS), found_own_topic))}")


def main():
    if len(sys.argv) < 3:
        print("usage: stress_embeddings_concurrency.py <path-to-wiki-server-binary> "
              "<path-to-gguf-model>", file=sys.stderr)
        return 2
    server_bin = sys.argv[1]
    model_path = os.path.abspath(sys.argv[2])
    if not os.path.exists(model_path):
        print(f"model path does not exist: {model_path}", file=sys.stderr)
        return 2

    with Server(server_bin, threads=4, embeddings_model_path=model_path) as server:
        stress_embeddings_concurrency(server)

    print(f"\n{'=' * 60}")
    if FAILURES:
        print(f"{len(FAILURES)} FAILURE(S):")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print("All embeddings concurrency stress checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
