# Semantic search (embeddings) — plan and status

Semantic search (`sqlite-vec` + embeddings, hybrid FTS5 BM25 + cosine ranking) was the
one Phase 2 item deliberately deferred at project start (see `docs/architecture.md`'s
milestone history) — no embedding-source decision had been made. This document is that
decision, plus the phased plan for building it.

**Current status: the entire planned pipeline is done and live-verified** — both
providers, `sqlite-vec` storage, hybrid ranking, and (as of this pass)
`LocalEmbeddingProvider` actually running inference on real production ARM hardware,
not just cross-compiled for it. The build-time options, the runtime config schema, the
`EmbeddingProvider` abstraction, `LocalEmbeddingProvider` (llama.cpp-backed),
`CloudEmbeddingProvider` (OpenAI `text-embedding-3-small`-backed), `EmbeddingIndexer`
(`sqlite-vec`/`vec0` storage), `IndexUpdater`'s embedding-on-write wiring, and
`FtsSearch`'s hybrid (BM25 + cosine, via Reciprocal Rank Fusion) ranking all exist and
have been verified against real inference, a real running server over real HTTP, and
real armv7l production hardware — see "Real-model verification", "Real-API
verification", "sqlite-vec storage — live end-to-end verification", "Hybrid ranking —
live end-to-end verification", and phased-rollout step 6 below. `wiki-mcp`'s write
tools now embed too, lazily (see "Closed gap" below); its `search_documents` tool
staying FTS5-only, never hybrid, remains a deliberate scope boundary, not an
oversight. `[embeddings].provider` defaults to `"none"`, and FTS5
remains the only search path in that mode — semantic search is additive to FTS5,
never a replacement for it.

## Real-model verification

`LocalEmbeddingProvider` was verified against a real GGUF model, not just built:
[`CompendiumLabs/bge-small-en-v1.5-gguf`](https://huggingface.co/CompendiumLabs/bge-small-en-v1.5-gguf)
(`bge-small-en-v1.5-f16.gguf`, 384-dim, BERT-family, MIT-licensed) — downloaded, loaded,
and run through real inference. `tests/unit/LocalEmbeddingProviderTest.cpp` (opt-in —
see "Testing against a real model" below) embeds several sentence pairs and checks
cosine similarity; observed numbers on this exact model:

| Pair | Relationship | Cosine similarity |
|---|---|---|
| "The cat sat on the mat." vs. itself | identical | 1.000 |
| "...cat sat on the mat" vs. "A kitten was resting on the rug" | related | 0.781 |
| "...cat sat on the mat" vs. "The stock market crashed yesterday" | unrelated | 0.437 |
| "Personal wiki with markdown storage..." vs. "A self-hosted note-taking app..." | related | 0.814 |
| "...markdown storage..." vs. "Quarterly financial report..." | unrelated | 0.528 |

A consistent ~0.28–0.34 gap between related and unrelated pairs, across two independent
sentence-pair sets — real evidence this produces usable semantic vectors, not just that
`embed()` returns *some* 384 floats without crashing. The test's assertions use
thresholds with real margin under these observed numbers (`related > 0.6`,
`unrelated < 0.6`, `related - unrelated > 0.15`) rather than pinning to the exact
figures, so a harmless model-version bump doesn't turn into test flakiness.

### A second real bug found verifying this: `GGML_NATIVE` can't be hardcoded OFF

`CMakeLists.txt`'s `WIKI_ENABLE_LOCAL_EMBEDDINGS` block originally hardcoded
`GGML_NATIVE OFF` unconditionally — correct reasoning for cross-compilation (ggml must
never probe the BUILD host's CPU when the RUN target is a different architecture) but
wrong for this same code path on an ordinary **native** x86_64 build. `GGML_NATIVE OFF`
made ggml pick one fixed CPU backend variant baked in at configure time (including
`-mavx2`) instead of the actual host's real feature set. This dev machine's CPU has
AVX/FMA/F16C but **not** AVX2 — the resulting binary passed every build step cleanly
and then crashed with `SIGILL` (`Illegal instruction`) on its first real `embed()`
call, caught only by actually running inference against a real model, not by the build
succeeding. Fixed by making it conditional: `OFF` only when `CMAKE_CROSSCOMPILING`,
`ON` otherwise — `ON` lets ggml probe and use whatever the current build host actually
supports, which is exactly the native-build case this needs.

### Testing against a real model

Not run by default (no model is vendored — GGUF files are tens of MB, not worth
carrying in-repo or downloading on every CI run for one provider's sanity check).
Opt in with both flags:

```sh
cmake -S . -B build -DWIKI_ENABLE_LOCAL_EMBEDDINGS=ON \
  -DWIKI_TEST_EMBEDDING_MODEL_PATH=/path/to/bge-small-en-v1.5-f16.gguf
cmake --build build --target local_embedding_provider_test
ctest --test-dir build -R local_embedding_provider_test --output-on-failure
```

## Real-API verification (`CloudEmbeddingProvider`)

`CloudEmbeddingProvider` (OpenAI `text-embedding-3-small`, 1536-dim) was verified
against the real OpenAI API, not just built against httplib. Real observed result:
`dimensions() == 1536`, a real 1536-float vector back from a real `embed()` call, and
`cosine(cat, kitten) > cosine(cat, "the stock market crashed yesterday")` — the same
related-vs-unrelated shape confirmed for the local provider, now confirmed for cloud
too, on a real network call, not a mock.

No new heavy dependency: reuses cpp-mcp's own vendored `httplib.h` (already fetched
for `wiki-mcp`), with `CPPHTTPLIB_OPENSSL_SUPPORT` defined locally in
`CloudEmbeddingProvider.cpp` only (cpp-mcp itself builds that header without SSL
support — it doesn't need outbound HTTPS) and linked against the OpenSSL this project
already requires elsewhere.

### Testing against the real API

Not run by default — this is a real network call against a real account's API key, not
something a build should silently attempt. Gated at build time
(`WIKI_ENABLE_CLOUD_EMBEDDINGS`) and, separately, skipped at **runtime** (via Catch2's
`SKIP`) when `OPENAI_API_KEY` isn't actually present in the environment — unlike the
local provider's model-file path, an API key is a secret this project can't require
CMake configure to see, so the runtime check is the only correct gate:

```sh
cmake -S . -B build -DWIKI_ENABLE_CLOUD_EMBEDDINGS=ON
cmake --build build --target cloud_embedding_provider_test
set -a && source .env && set +a   # or otherwise export a real OPENAI_API_KEY
ctest --test-dir build -R cloud_embedding_provider_test --output-on-failure
```

## `sqlite-vec` storage and embedding-on-write

`EmbeddingIndexer` (`src/index/EmbeddingIndexer.{h,cpp}`) owns the `sqlite-vec`-backed
`document_embeddings` virtual table — `ensureTable(dimensions, modelIdentifier)` (creates
it, or drops and recreates it if the configured provider's dimensionality OR model
identity changed since last run — see "vec0's dimension is fixed at CREATE time" and
"Skip-if-unchanged and self-healing" below), `upsertOne`/`removeOne`, and
`nearest()` (KNN via `vec0`'s own `MATCH`/`ORDER BY distance` query shape). Vendored via
`FetchContent` + pinned commit (same discipline as `llama.cpp`/`cpp-mcp`), compiled
directly into `wikicore` and registered once per process via
`sqlite3_auto_extension()` in `Database.cpp` — see that file's own comment on the real
`sqlite3_api` macro-pollution bug hit and fixed while wiring this up (below).

`IndexUpdater::upsertOne`/`removeOne` call into `EmbeddingIndexer` automatically
whenever an `EmbeddingProvider*` is passed to their constructor (optional, defaults to
`nullptr` — every existing call site that doesn't pass one is unaffected). `main.cpp`
constructs the configured provider once at startup (right after `--create-admin`'s
early return, before `--reindex`) and passes it to every `IndexUpdater` instance,
including `VaultWatcher`'s own separate one — so a document changed by an external
editor/`git pull` gets embedded too, not just documents saved through the Web UI.

### vec0's dimension is fixed at CREATE time — a provider swap loses stored vectors

Confirmed against `sqlite-vec`'s own examples: `CREATE VIRTUAL TABLE ... USING
vec0(embedding float[N])` takes `N` as a literal in the DDL, not a bind parameter —
there's no way to make one `vec0` table accept both the local provider's 384-dim
vectors and the cloud provider's 1536-dim ones. `EmbeddingIndexer::ensureTable()`
tracks the table's current width in `index_meta` (the same key/value table
`schema_version` already lives in) and, on a mismatch, drops and recreates the table
empty. This is an accepted cost, not a bug to route around: the whole index (this
table included) is already documented project-wide as fully disposable and
rebuildable from the vault (`docs/architecture.md`'s storage model) — exactly `--reindex`'s
job, which now recomputes embeddings too (see above).

### Three real bugs found wiring this together

**`sqlite3_api` macro pollution.** `sqlite-vec.h` includes `sqlite3ext.h` unless
`SQLITE_CORE` is defined — and `sqlite3ext.h` redefines every `sqlite3_*` symbol as a
macro through a `sqlite3_api` diversion table, intended for the loadable-extension
file itself (`sqlite-vec.c`, where `sqlite3_vec_init()` genuinely does initialize that
table from its `pApi` parameter). `Database.cpp` doesn't need that whole header — it
just needs `sqlite3_vec_init`'s prototype to pass to `sqlite3_auto_extension()` — but
naively `#include <sqlite-vec.h>` there pulled in the same macros anyway, silently
breaking every ordinary `sqlite3_exec`/`sqlite3_open_v2`/etc. call **elsewhere in that
same file** with `'sqlite3_api' was not declared in this scope` compile errors. Fixed
by hand-declaring just the one function this file actually needs
(`extern "C" int sqlite3_vec_init(...)`) instead of including the header that comes
with that side effect — see `Database.cpp`'s own comment for the full reasoning.

**A lock-scoping bug caught by REASONING before it shipped, not by a crash.** The
first draft of `IndexUpdater::upsertOne`'s embedding step ran
`provider_->embed(...)` — a real network call for the cloud provider, real model
inference for local — **while still holding `mutex_`**, the same lock every
concurrent document/FTS save on this `IndexUpdater` needs. That would have
serialized every write the whole process makes behind one slow embedding call,
functionally re-introducing the shared-connection contention this project's own
`IndexUpdater` mutex was originally added to prevent, just relocated to a slower
resource. Restructured so `provider_->embed()` runs with no lock held at all, and
only the (fast) `EmbeddingIndexer` write afterward takes a lock — a SEPARATE
`embeddingMutex_`, not the original `mutex_`, needed because two threads finishing
`embed()` around the same time and both then racing a `BEGIN` on the shared `db_`
connection would reproduce the identical real concurrency bug `mutex_` itself exists
to prevent (see `IndexUpdater.h`'s own comments on both mutexes). Caught by tracing
through the locking implications before writing the implementation, not by a stress
test catching it live the way the original `IndexUpdater` race was — worth recording
as a case where the earlier incident's lesson (documented in `docs/architecture.md`
and `CLAUDE.md`) got applied proactively instead of needing to be rediscovered.

**The default `provider = "none"` config, on a `WIKI_ENABLE_SQLITE_VEC` build,
was silently poisoning every document with a fake failure record.**
`EmbeddingProviderFactory::createEmbeddingProvider()` always returns a real,
non-null `NullEmbeddingProvider` for `provider = "none"` rather than `nullptr` —
"none" is meant to stay a valid, always-buildable choice on its own (see
`NullEmbeddingProvider.h`). But `IndexUpdater`/`FtsSearch` both treat a raw
`nullptr` as THEIR OWN "no provider configured" signal (see their own header
comments — `provider_ != nullptr` gates the whole embedding step). `main.cpp`
was handing them `embeddingProvider.get()` directly at every call site — a live
`NullEmbeddingProvider*`, never `nullptr` — so on any binary built with
`WIKI_ENABLE_LOCAL_EMBEDDINGS`/`WIKI_ENABLE_CLOUD_EMBEDDINGS` and left at its
default config (no `[embeddings]` section at all, or an explicit
`provider = "none"`), every single document save actually attempted
`provider_->embed()`, which `NullEmbeddingProvider::embed()` always throws by
design — silently recording a permanent `document_embedding_state` failure row
for every document, even though the admin never asked for semantic search in
the first place. Found while designing the admin "needing attention" list
(below): that list would have shown every document as broken on a plain
`WIKI_ENABLE_LOCAL_EMBEDDINGS` build with no real embeddings configured, which
would have been actively misleading rather than merely incomplete. Fixed in
`main.cpp` by computing one `activeEmbeddingProvider` pointer right after
constructing `embeddingProvider` —
`embeddingProvider->modelIdentifier() == "none" ? nullptr : embeddingProvider.get()`
— and using that at every call site (`IndexUpdater`, `FtsSearch`, the
`VaultWatcher` path, and `--reindex`) instead of `embeddingProvider.get()`
directly. Verified live: a real `wiki-server` (built with
`WIKI_ENABLE_LOCAL_EMBEDDINGS`, no `[embeddings]` section in `config.toml`)
saving a real document over real HTTP now leaves `document_embedding_state`
at zero rows and never even creates the `document_embeddings` `vec0` table —
before the fix it would have created a `float[0]` vec0 table (a second,
downstream bit of nonsense from `dimensions() == 0`) and one failure row per
save.

### Live end-to-end verification (not just unit tests)

Beyond `tests/unit/EmbeddingIndexerTest.cpp` (storage in isolation) and
`tests/unit/IndexUpdaterEmbeddingTest.cpp` (the real-model, real-`IndexUpdater` wiring
— includes a genuinely-failed-then-fixed assertion: an early version expected a
document's own body text to embed near-identically to its stored vector, and got
distance ≈0.05 instead of ≈0 — turned out the query text didn't match what
`IndexUpdater` actually embeds, `title + "\n\n" + body`, not the body alone; a real
empirical finding about this model, not test flakiness), this was verified against a
**real running `wiki-server` process**, not just test binaries:

1. Started `wiki-server` with a real `config.toml` (`embeddings.provider = "local"`,
   a real `model_path`) — started clean, no crash loading the model at boot.
2. Logged in and created a real document (`POST /api/documents`,
   `"The cat sat on the mat."`) over real HTTP, with a real CSRF token.
3. Confirmed via `sqlite3`'s own CLI that `document_embeddings` (and its `vec0` shadow
   tables — `_chunks`, `_rowids`, `_vector_chunks00`) exist in the real `.index.db`
   file on disk, and `index_meta`'s `embedding_dimensions` reads `384` — the system
   `sqlite3` binary can't actually query INTO `document_embeddings` itself (`no such
   module: vec0` — `vec0` is compiled statically into `wiki-server` alone, not
   available to an unrelated CLI tool), which is itself confirmation the extension is
   real and specific to this binary, not a generic SQL error.
4. Wrote and ran a small standalone probe (linked against the same `libwikicore.a`)
   against that exact database file, querying `nearest()` with the semantically
   related but **lexically unrelated** phrase `"A small feline animal."` (no word in
   common with the stored `"The cat sat on the mat."`) — it found the stored document
   (`distance ≈ 0.296`), real evidence of semantic (not keyword) matching working
   through the full real stack: HTTP → `DocumentService` → `IndexUpdater` →
   `LocalEmbeddingProvider` → `EmbeddingIndexer` → `vec0` storage → KNN query.

### Closed gap: `wiki-mcp`'s write tools now embed too, lazily

`wiki-mcp` (`src/mcp_main.cpp`) constructs an `IndexUpdater` for its Phase 2
`create_document`/`update_document` tools, but deliberately does NOT construct a
real `EmbeddingProvider` at process startup — `wiki-mcp` is spawned fresh per MCP
session and documented project-wide as staying "fast-starting, dependency-light"
(see `CLAUDE.md`'s two-binary-layout entry); unconditionally constructing a
provider at startup would mean every MCP session pays for loading a local model
(or validating a cloud API key) even for a purely read-only session that never
calls a write tool — the overwhelmingly common case (`search_documents`/
`get_document`/`list_tags`/`list_documents`).

Instead, `mcp/McpServer.cpp`'s `LazyEmbeddingProvider` constructs the real
provider (via `EmbeddingProviderFactory`, same as `wiki-server`) on the FIRST
actual `create_document`/`update_document` call, via
`IndexUpdater::setProvider()` (a small setter added specifically for this —
`wiki-server` never uses it, since it already knows its provider up front and
passes it straight to the constructor). Every write after the first reuses the
already-loaded provider; a read-only session never touches any of this. A failed
lazy-init (bad model path, missing API key) is logged to stderr once and the
write proceeds FTS5-only — same best-effort philosophy as `wiki-server`'s own
embedding step, an embeddings problem must never fail the document save itself.

Verified live over real MCP stdio (not just a unit test in isolation): a
`tools/list` call produces zero llama.cpp loading output in stderr (the provider
genuinely wasn't constructed yet); the first `create_document` call DOES produce
real `llama_decode`/inference log lines, and the created document's
`document_embedding_state` row shows a real `content_hash` with no `last_error`
— a real, successful embed, not a silently-skipped one.

`wiki-mcp`'s `FtsSearch` (its `search_documents` tool) still does NOT receive a
provider, and stays FTS5-only rather than hybrid — that half of the original gap
remains open on purpose: unlike the write path, there's no natural "first write"
moment to lazily hook into for a read-only search tool, and wiring a provider in
just for this would reopen the exact "every session pays for a model load"
problem this section describes, just for reads instead of writes.

## Skip-if-unchanged and self-healing

The first version of this feature re-embedded every document on every
`IndexBuilder::fullRescan()` — which runs unconditionally at every `wiki-server`
startup — even when nothing had changed since the last successful embed. On a
resource-constrained SBC with a local model, that's real, measured cost (see
"Deployed to production" below): every restart pays for every document's inference
again, serially, before the HTTP port even opens.

`document_embedding_state` (`src/index/schema.h`'s `kMigration5`) tracks, per
document row, the `content_hash` (a plain `std::hash<std::string>` of
`title + "\n\n" + body` — no cryptographic strength needed, there's no adversarial
model here; a hash collision would at worst skip a re-embed that a later edit would
still trigger) as of its **last successful** embed, and `last_error` (`NULL` exactly
when that attempt succeeded). `IndexUpdater::upsertOne` computes the current hash,
asks `EmbeddingIndexer::needsEmbedding()` whether it matches, and only calls
`provider_->embed()` — the slow, real-inference-or-network step — when it doesn't.

**Failure doesn't touch `content_hash`, only `last_error` — deliberately.** If
`embed()` throws (model unreachable, cloud API down, whatever), `content_hash` keeps
whatever value it held from the last successful embed. Two consequences, both
intended: an old, still-accurate vector for unchanged old content isn't invalidated
by an unrelated later failure; and a document whose content genuinely changed but
whose embed attempt failed stays permanently mismatched against its (stale)
recorded hash, so `needsEmbedding()` keeps returning `true` and the very next
rescan or save retries automatically — no separate "pending" queue to manage, no
lost retries. This surfaced as a real, useful finding while writing
`EmbeddingIndexerTest.cpp`: a test originally asserted the OPPOSITE (that a later
failure should invalidate the earlier successful hash) and failed — the test's
expectation was wrong, not the code, and got corrected along with its comment.

**A model swap forces a full re-embed, even at unchanged dimensionality.** Content
hashing alone can't catch this: two different embedding models can both produce
384-dim vectors while encoding genuinely incompatible vector spaces for the same
text — comparing a stored vector from one against a fresh query from the other
produces a meaningless distance, silently, not an error. `EmbeddingProvider` gained
a `modelIdentifier()` pure virtual (`"local:<path>:<mtime>:<size>"` for
`LocalEmbeddingProvider` — file identity, not just its path, so swapping the file
at the same configured path is also caught; `"cloud:<model-name>"` for
`CloudEmbeddingProvider`) that `EmbeddingIndexer::ensureTable()` now compares
alongside `dimensions()`. A mismatch on EITHER drops and recreates
`document_embeddings` AND clears every row of `document_embedding_state` — a
document whose text never changed still needs a real re-embed against the new
model, not a skip, because its hash matching means nothing once the vector space
itself has changed underneath it. Accepted cost, not a bug: identical posture to
"vec0's dimension is fixed at CREATE time" above — a model swap already implied
"index is now stale everywhere," this just makes the code actually detect it.

**Admin visibility for documents needing attention.** `EmbeddingIndexer::listNeedingAttention()`
returns every document with no `document_embedding_state` row at all (never
attempted — e.g. embeddings were just enabled, or a model swap just cleared every
row) or whose last attempt failed, ordered by path. Deliberately excludes documents
that are merely due for a routine hash-mismatch re-embed on next save — this list is
specifically "something needs a human to look at it," not "this is due for normal
work." See "Admin visibility and control" below for how this gets surfaced and
acted on without a full `--reindex`.

Proven end-to-end, not just in `EmbeddingIndexerTest.cpp`'s isolated storage tests:
`tests/unit/IndexUpdaterEmbeddingTest.cpp` wraps a real `LocalEmbeddingProvider` in a
call-counting decorator and drives it through the real `IndexUpdater::upsertOne()`
path — confirms a second save with byte-identical content triggers zero additional
real `embed()` calls, and a genuinely changed body on the third save triggers exactly
one more, real proof the skip logic isn't a false positive that also swallows real
changes.

## Admin visibility and control

Two runtime-mutable pieces, both SQLite-backed (no `config.toml`/restart involved,
same discipline as `auth::McpRemoteConfig`), plus an admin API surfacing the
self-healing state above:

- **`EmbeddingsRuntimeConfig`** (`src/index/EmbeddingsRuntimeConfig.{h,cpp}`,
  `embeddings_runtime_config` — a singleton row, same `CHECK(id = 1)` pattern as
  `mcp_remote_config`): a plain `vectorSearchEnabled` bool. `FtsSearch::search()`
  reads it on EVERY call, not just at construction — an admin can kill hybrid
  ranking live (a misbehaving provider, a cost concern for the cloud provider,
  whatever the reason) without restarting `wiki-server`, and turn it back on the
  same way. Verified live: the same `FtsSearch`/provider instance, same query,
  found a lexically-unrelated-but-semantically-related document with the toggle
  on, missed it with the toggle off, found it again after flipping it back on —
  proof `search()` genuinely re-checks the flag per call rather than caching a
  decision made at construction.
- **`GET/POST /api/admin/embeddings-status`** (`src/controllers/AdminRoutes.cpp`,
  admin+CSRF, only registered on a `WIKI_ENABLE_SQLITE_VEC` build — see that
  file's header comment for the exact request/response shapes): `GET` returns
  whether a real provider is configured, its dimensions/model identity, the
  runtime toggle's current state, and `EmbeddingIndexer::listNeedingAttention()`
  as-is. `PUT` flips the toggle. `POST .../reembed` re-derives one document's
  whole index row (via `IndexBuilder::reindexOneFile`, the same primitive
  `VaultWatcher` uses) — NOT a full `--reindex`. `POST .../reembed-all` does that
  for every currently-listed document and reports how many still fail afterward,
  rather than claiming success it didn't actually verify. Both are logged to
  `mcp_audit_log` (`"admin:reembed"`/`"admin:reembed-all"` — same table and
  `"admin:"`-prefix convention `RemoteMcpRoutes.cpp` already uses for the HTTP
  MCP transport's own `"remote:"` prefix), success or failure, visible via the
  existing `GET /api/admin/mcp-audit-log` — an admin retry is a real
  write-adjacent action, and "what happened to this vault, and when" is already
  exactly what that log answers; no reason to build a second one for the same
  question. `reembed-all` writes ONE summary entry per call, not one per
  document — the response body's own `attempted`/`stillFailing` counts already
  say which.
- **Account page UI** (`static/js/pages/account.js`, "Semantic search
  (embeddings)" section, same apply-immediately style as the Remote MCP section
  right above it — no separate "Save" button): a checkbox bound to the runtime
  toggle, and a "Documents needing attention" list with a per-document Retry
  button and a Retry-all button. The section fetches `GET
  /api/admin/embeddings-status` on page load and simply doesn't render itself
  at all on a `404` (a build without embeddings compiled in), rather than
  showing controls that would 404 on every click. Verified live in a real
  browser against a real `wiki-server` (local provider, a real oversized
  document deliberately triggering the crash-bug-turned-clean-failure below):
  toggling the checkbox actually flips `EmbeddingsRuntimeConfig` (confirmed by
  reading it back via the API afterward), clicking Retry on the oversized
  document re-attempts and correctly shows the same failure again, clicking
  Retry all reports `"Retried 1 document(s); 1 still failing"`, and shrinking
  the document via a normal edit makes it disappear from the list on the next
  page load with no manual action — the browser's own console showed zero
  errors from this page's own code throughout.

The `provider = "none"` fake-failure bug described in "Three real bugs" above
was actually caught DESIGNING this `needingAttention` list — before the fix,
this endpoint would have shown every document as "broken" on any
`WIKI_ENABLE_SQLITE_VEC` build left at its default config, which would have
been actively misleading rather than merely incomplete.

### A fourth real bug, found testing this same admin path: an oversized document crashed the whole server, not just its own embed attempt

Feeding `LocalEmbeddingProvider::embed()` a document long enough to tokenize past
the model's own `n_ubatch` reached `llama_decode()` and tripped an internal
`GGML_ASSERT(cparams.n_ubatch >= n_tokens)` — llama.cpp's assert macro calls
`abort()` directly, a hard process crash with no C++ exception for
`IndexUpdater`'s existing `catch (...)` to intercept. Reproduced live: a real
`wiki-server` (real HTTP, real admin session) saving one ~5000-word document
took the entire process down with `SIGABRT`, and — worse — because
`IndexBuilder::fullRescan()` runs unconditionally at every startup, simply
restarting would have hit the exact same document again and crash-looped
forever. Fixed in `LocalEmbeddingProvider`: `embed()` now checks the tokenized
length against `maxTokens_` (the context's own `n_ctx`, captured once at
construction) and throws a normal `std::runtime_error` BEFORE calling
`llama_decode()` if it's exceeded — not just crash-prevention: a text longer
than the model's trained context window produces a semantically meaningless
embedding even when it technically doesn't crash, so rejecting it outright is
also the semantically correct behavior, not merely the safe one. Re-verified
live end to end after the fix: the identical oversized document now shows up
in `needingAttention` with a clear `"...exceeding this model's 512-token
context window..."` message instead of taking the server down, `POST
.../reembed` on it correctly returns `{"ok":true}` while the underlying embed
still (correctly) fails again, and shrinking the document via a normal edit
makes it disappear from the list on its own — the exact self-healing path this
whole feature was built for, now provably correct for this failure mode too.
`tests/unit/LocalEmbeddingProviderTest.cpp` has a permanent regression test:
an oversized text throws `std::runtime_error` (not a crash), and the SAME
provider instance stays usable for a normal-length text immediately after.

## Two more real concurrency bugs, found cross-compiling for a production deploy

Both found the same way as the earlier bugs above: not by reasoning about the
code, but by actually running it under load before shipping. Neither ever hit
real production — both were caught in pre-flight verification.

### A fifth real bug: concurrent `embed()` calls on the SAME provider instance SIGSEGV'd

`IndexUpdater::upsertOne()` deliberately calls `provider_->embed()` outside any
lock (a slow embed must never block other threads' document saves — see that
method's own comment), and `main.cpp` shares ONE `LocalEmbeddingProvider`
instance across every `IndexUpdater` in the process (the HTTP-request one AND
`VaultWatcher`'s own separate one). Nothing was serializing two threads calling
`embed()` on that same instance at once — `llama_context`'s mutable KV-cache/
sequence state (`llama_memory_clear`/`llama_decode` both write into it) isn't
reentrant. Reproduced live under `qemu-arm-static`: two threads each calling
`embed()` once on one provider instance produced a hard `SIGSEGV` on the very
first run, no flakiness needed to trigger it. Fixed with a mutex owned by
`LocalEmbeddingProvider` itself (`embedMutex_`, guarding the WHOLE function,
not just the `llama_decode()` call — the vocab/tokenize steps read `model_`,
which is safe to share, but locking only part of the function would just move
the race instead of removing it). `CloudEmbeddingProvider` needed no such fix
— it constructs a fresh `httplib::Client` per call and touches no shared
mutable state at all.

### A sixth real bug: two INDEPENDENT mutexes guarding the SAME sqlite3 connection

Fixing the fifth bug made the isolated provider safe, but a NEW stress test
(`tests/integration/stress_embeddings_concurrency.py` — concurrent
`POST`/`PUT /api/documents` against a real server with real local embeddings,
proving the whole stack, not just the isolated provider class) still
occasionally produced a real `500`:
`{"error":"statement failed: cannot start a transaction within a transaction"}`
— the exact same class of bug `IndexUpdater::mutex_` was already built to
prevent (see that member's own comment, and `docs/architecture.md`'s
`IndexUpdater` postmortem), just via a different pair of call sites. The
embedding step had its OWN, separate `embeddingMutex_`, added specifically so
a slow `embed()` call never blocks a document/FTS save waiting on `mutex_`.
That reasoning about `embed()` itself was right — but `EmbeddingIndexer::
ensureTable()` runs its OWN `BEGIN IMMEDIATE...COMMIT` (whenever the
dimensions/model don't already match — i.e. the first document embedded
against a fresh database, or right after a model swap), and having it guarded
by a DIFFERENT mutex than the one guarding document/FTS transactions meant a
thread mid-`ensureTable()` (holding `embeddingMutex_`) and a thread mid-
document-save (holding `mutex_`) could both run `BEGIN` on the SAME `sqlite3*`
connection at once — two independent mutexes, neither aware of the other,
protecting the same underlying resource. Reproduced live: about 1 in 9 runs
of the new concurrent-create stress test against a fresh sandbox database (the
exact condition where `ensureTable()`'s `BEGIN` actually fires, rather than
its usual no-op fast path). Fixed by consolidating onto ONE mutex
(`IndexUpdater::mutex_`) for every `BEGIN`/`COMMIT` this class runs, embedding-
related or not — `embeddingMutex_` was removed entirely. `provider_->embed()`
itself is still the only thing that runs fully unlocked; every actual database
write, whichever table it touches, now shares the same lock. Re-verified with
15 consecutive clean runs of the stress test post-fix (zero failures, where
pre-fix reproduction needed under 9 runs on average) — see
`tests/integration/stress_embeddings_concurrency.py`'s own module docstring
for exactly what it drives and why the isolated `LocalEmbeddingProviderTest.cpp`
concurrency test alone wasn't enough to catch this second bug.

## Non-blocking startup rescan

`IndexBuilder::fullRescan()` (unconditional at every `wiki-server` startup —
see the storage-model section of `docs/architecture.md`) now runs on a
background `std::thread` instead of blocking `main()` before the HTTP
listener opens. `main()` joins it after `drogon::app().run()` returns (i.e.
on shutdown), so a still-running rescan finishes cleanly instead of racing
process teardown.

**Why this matters less than it sounds, most of the time.** Skip-if-unchanged
content-hash tracking (above) already makes a routine restart with no real
content changes fast on its own — measured: 20 unchanged documents took 3.1s
cold (first-ever migration) vs 0.2s warm (zero `embed()` calls, every hash
matched). The real, still-slow case is narrower: the first-ever enable of
embeddings on an existing vault, a model swap (clears every document's
tracked state), or a bulk import while the server was down — genuine,
per-document model inference with nothing to skip. On the real ARM
production target that's measured in tens of seconds, not milliseconds —
which used to mean blocking the HTTP listener (and therefore every route)
behind it, a real `nginx 502` window for anyone hitting the site during a
routine embeddings rollout.

**Why running it concurrently with live HTTP traffic is safe, not just
convenient.** The background rescan uses the SAME `indexUpdater`/`db` that
HTTP request handlers (`DocumentService` et al.) already share —
`IndexUpdater::mutex_` (see "Two more real concurrency bugs" above)
serializes every `BEGIN`/`COMMIT` this class runs against that connection
regardless of which thread calls it, so a document saved over HTTP while the
rescan is still mid-vault just gets indexed immediately by that save; the
rescan either confirms the same row again when it gets there, or never
touches a document that didn't exist yet when it started walking the vault
— the identical incremental behavior `VaultWatcher` already provides today,
just for the startup case too. Search results are correspondingly
incremental during this window (visibly filling in as the rescan
progresses) rather than the whole site being unreachable until it
finishes — strictly better, not a new kind of incompleteness.

Verified live, both locally and under `qemu-arm-static`: HTTP port opens in
well under a second even with dozens of documents queued for real embedding
in the background (previously would have blocked for the full embedding
duration); a document created over HTTP WHILE the background rescan is still
running succeeds cleanly (`201`, no error, no crash); once the rescan
finishes, `GET /api/admin/embeddings-status` reports zero documents needing
attention — every one, rescan-created and HTTP-created alike, ended up
correctly embedded; a `SIGTERM` sent immediately after startup still shuts
down cleanly (the join waits for the in-flight rescan rather than tearing
down `indexBuilder`/`db` out from under it).

`--reindex` (the CLI flag) and `POST /api/admin/reindex` both stay fully
synchronous on purpose — a caller that explicitly asked for a reindex wants
to know when it's actually done, unlike this one-time startup pass nobody
is blocking on.

### Progress feedback: "is it done yet, and how far did it get"

Running the rescan in the background solved the blocking problem, but
introduced a new one: with no visible port-not-ready signal, "is it done
yet" was answerable only by tailing server logs. `RescanProgress`
(`src/index/RescanProgress.h`) is a tiny all-atomic struct (`inProgress`,
`documentsIndexed`) — no mutex needed, since each field is independently
meaningful without needing to be read as a consistent snapshot together.
`IndexBuilder::fullRescan()` takes an optional `RescanProgress*` (`nullptr`
by default — the CLI `--reindex` path passes none, since it already prints
its own final `RescanStats` synchronously and has no concurrent reader to
report to), incrementing `documentsIndexed` after each file and clearing
`inProgress` on the way out via an RAII guard (so a rescan that throws
partway through doesn't leave the tracker permanently claiming "still
running"). One `RescanProgress` instance in `main.cpp` is shared across
all three rescan paths — the background startup rescan, `POST
/api/admin/reindex`, and (implicitly, since it's the same process-wide
instance) whichever one most recently ran.

`GET /api/admin/reindex-status` (`AdminRoutes.cpp`, admin-only, no CSRF —
a pure read of in-memory atomics) exposes it: `{"inProgress":bool,
"documentsIndexed":N}`. The account page (`static/js/pages/account.js`)
polls it every 1.5s and shows a single self-hiding status line —
`"Reindexing in progress — N document(s) so far."` — only while
`inProgress` is true, adding no visual noise the overwhelming majority of
the time a restart's background rescan has already finished before anyone
loads the page. Registered on every build (unlike the embeddings-status
section below it), since a full-vault rescan is relevant with or without
embeddings configured.

Verified live: polling `GET /api/admin/reindex-status` once every ~0.4s
during a real 30-document background startup rescan showed
`documentsIndexed` climbing (`1 → 5 → 9 → 14 → 18 → 22 → 26 → 30`) with
`inProgress:true` throughout, then `inProgress:false` exactly once it hit
30 — a real, live-updating count, not a static placeholder. A subsequent
`POST /api/admin/reindex` against the same (now fully indexed) vault
correctly reported `documentsIndexed:30` in both its own synchronous
response and the progress endpoint immediately after.

## Hybrid ranking — live end-to-end verification

`FtsSearch::search()` (text-search mode only — browse mode has no query text to
embed) blends BM25 lexical ranking with cosine-distance semantic ranking via
**Reciprocal Rank Fusion (RRF)**: `bm25CandidateRowIds()` and
`EmbeddingIndexer::nearest()` each independently rank up to 200 candidate rowids;
RRF (`score += 1/(60 + rank)` per list, `k=60` — the standard constant from the
original RRF paper, not tuned for this project) combines them into one ranking
without needing to normalize BM25 scores and cosine distances onto a shared scale,
since RRF only ever looks at rank POSITIONS. `limit`/`offset` apply to the merged
ranking, not either source list. A rowid that came only from the semantic side (no
FTS5 MATCH at all) falls back to the stored excerpt instead of `snippet()` (which has
no defined result outside a MATCH context) — `fetchByRowIds()` splits the final page
into snippet-eligible and excerpt-only groups and re-sorts the combined result back
into RRF order (SQL's `IN (...)` gives no ordering guarantee).

Falls back to plain FTS5-only ranking (today's pre-hybrid behavior, byte-for-byte)
whenever hybrid ranking can't proceed: no provider configured, no
`document_embeddings` table yet, or `provider_->embed()` itself failing — same
best-effort, additive-never-load-bearing posture as `IndexUpdater`'s own embedding
step.

Verified two ways:

**Real-model tests** (`tests/unit/FtsSearchHybridTest.cpp`, same opt-in gate as the
other real-model tests): confirmed, against `bge-small-en-v1.5`, that (1) plain FTS5
genuinely does NOT find a document via a query sharing zero words with it (the
baseline hybrid search improves on), (2) hybrid search WITH a provider DOES find that
same document via that same lexically-unrelated query, ranked first, (3) an exact
keyword match still works and still ranks first through the hybrid path — adding
semantic ranking doesn't regress plain lexical search, (4) hybrid search falls back
to FTS5-only cleanly when `document_embeddings` doesn't exist yet. All four passed on
the first real run.

**Real HTTP end-to-end**: started a real `wiki-server` (`embeddings.provider =
"local"`), created two real documents over `POST /api/documents` — one about a cat,
one about a financial report — then queried `GET /api/search?q=A+small+feline+animal`
(zero words in common with either document) over real HTTP. Result: the cat document
ranked first, the unrelated finance document second, `snippetIsHighlighted: false` on
the result (confirming it came through the semantic/excerpt path, not an FTS5 MATCH)
— the same real search a browser or the MCP `search_documents` tool would see,
observably better than FTS5-only search could produce for that query.

### A real relevance bug, found from a real user report on real production content

A one-word search ("stabilize") on the live wiki returned most of the vault —
recipes, a welcome page, empty demo documents — alongside the couple of
genuinely relevant results. Two compounding root causes, both real, both fixed
together:

**1. `EmbeddingIndexer::nearest()` has no relevance floor.** It returns the N
closest neighbors, full stop — on a small vault (the common case for this
project), "the N closest neighbors" is effectively the WHOLE vault, ranked by
a distance that's often just noise for a document with nothing to do with the
query. RRF then gives every one of them a nonzero score regardless of actual
relevance — a candidate, once in a ranked list, has no way to be rejected by
RRF itself, only ranked.

**2. The query side was embedded exactly like a document.** bge-small-en-v1.5
— like other small local retrieval models — is trained with an instruction
prefix on the QUERY side only; its own model card recommends
`"Represent this sentence for searching relevant passages: "`. Embedding a
bare query the same way a passage gets embedded measurably narrows the gap
between relevant and irrelevant results, which matters doubly once a distance
threshold is added: a threshold can't cleanly separate two things that are
already close together.

Real measurements against bge-small-en-v1.5, query `"stabilize"`, cosine
distance (0 = identical, 2 = opposite) to a handful of real documents:

| document | distance, no prefix | distance, with prefix |
|---|---|---|
| API Stability (relevant) | 0.3415 | 0.3692 |
| Move Semantics (relevant-ish) | 0.3988 | 0.3884 |
| Smart Pointers (borderline) | 0.4586 | 0.4657 |
| Borscht (irrelevant) | 0.4633 | 0.5047 |
| Pasta Carbonara (irrelevant) | 0.4887 | 0.5431 |
| Welcome (irrelevant) | 0.4535 | 0.5855 |

Without the prefix, Borscht (0.4633) sits BELOW Smart Pointers (0.4586) — no
single distance threshold separates relevant from irrelevant cleanly. With the
prefix, every irrelevant document lands at 0.50+ while every relevant one
stays under 0.47 — a real, usable gap for a threshold to sit in.

**The fix, two parts:**
- `EmbeddingProvider::embedQuery()` (new, default = `embed()`) — overridable
  per-provider for an asymmetric retrieval model.
  `LocalEmbeddingProvider::embedQuery()` prepends a configurable
  `queryPrefix_` (empty by default — no behavior change unless
  `embeddings.query_prefix` is actually set in `config.toml`).
  `FtsSearch::tryHybridSearch()` calls `embedQuery()`, never `embed()`, for
  the query side. `CloudEmbeddingProvider` needed no change — OpenAI's
  embeddings are symmetric, the default (`embedQuery() == embed()`) is
  already correct.
- `FtsSearch`'s new `maxSemanticDistance_` (from `embeddings.max_distance`,
  default `0.5` — chosen from the measurements above, not guessed) filters
  `nearest()`'s results BEFORE they ever reach RRF. A document whose distance
  exceeds it never becomes a semantic candidate, however close it happens to
  be relative to everything else in a small vault.

Verified live against a sandbox reproducing the real reported content
(the same recipes/welcome/demo-doc mix): before the fix, searching
`"stabilize"` returned 10 of 10 seeded documents; with `max_distance = 0.5`
alone (no query prefix), Borscht and Pasta Carbonara still leaked through
(their un-prefixed distances, 0.4633/0.4887, sit under 0.5); with both fixes
together, exactly the relevant/borderline documents remained and every recipe/
welcome/demo document was excluded. `tests/unit/LocalEmbeddingProviderTest.cpp`
and `tests/unit/FtsSearchHybridTest.cpp` both have permanent regression tests
built on these same real measurements.

### A third root cause, found re-checking the SAME bug on more real queries: a distance threshold alone doesn't fix a document whose OWN vector is weak

The two fixes above shipped, then a follow-up check against other real
production queries ("concurrent coroutines", "ownership pointer memory",
"simmer soup", "scheduled cron automation" — none related to the original
"stabilize" report) found `welcome.md` STILL leaking into results for several
of them. Measuring the real `welcome.md` content
(`"# Welcome\n\n![Brizon060.webp](...)"` — a title plus an image link, four
words total) against those exact queries with the query-prefix fix already
applied:

| query | welcome.md distance |
|---|---|
| stabilize | 0.5254 (correctly excluded) |
| concurrent coroutines | 0.4921 (leaked) |
| ownership pointer memory | 0.4975 (leaked) |
| simmer soup | 0.4951 (leaked) |
| scheduled cron automation | 0.4807 (leaked) |

A document with almost no prose doesn't carry a strong enough semantic signal
to land reliably far from an arbitrary unrelated query — its vector sits in
an uninformative middle region of the embedding space that happens to fall
under `max_distance` for SOME queries and not others, essentially at random.
No amount of tuning the threshold fixes this: the problem isn't where the
cutoff sits, it's that the vector itself doesn't carry enough information to
be reliably on one side of any cutoff.

**The fix**: `IndexUpdater::upsertOne()` now skips the `embed()` call
entirely for a document whose title+body combined has fewer than
`embeddings.min_content_words` (default `6`, whitespace-separated tokens —
chosen to exclude the real 4-word `welcome.md` stub while keeping an 8-word
test sentence like "About cats" / "The cat sat on the mat." eligible) — see
`AppConfig::embeddingsMinContentWords`. Skipped, not failed: it records
success-with-no-vector (so it never shows up in the admin "needing attention"
list — that list means "something's broken", not "this is fine, just
short") and removes any STALE vector from a previous, longer version of the
same document. A later edit that adds enough real content re-triggers a real
embed via the normal content-hash mismatch — this self-heals exactly like
every other skip-if-unchanged case in this file.

**A fourth bug, found building the THIRD fix, caught before shipping**: the
first version of the skip path recorded success directly, without calling
`EmbeddingIndexer::ensureTable()` first. If a too-short document was the
FIRST one ever processed (a real, easy-to-hit ordering — filesystem
directory iteration makes no ordering guarantee), `index_meta`'s
dimensions/model stay completely unset at that point. The next REAL
document's own `ensureTable()` call then sees "no recorded dimensions" as a
model MISMATCH — the identical signal a genuine model swap gives — and wipes
the ENTIRE `document_embedding_state` table as a side effect of establishing
the schema for the first time, silently erasing the too-short document's
row that had just been written. Reproduced live: a two-document sandbox
(`welcome.md` first, a real document second) showed the too-short document's
`document_embedding_state` row present immediately after being written, then
gone by the time the second document finished processing — caught by adding
temporary diagnostic logging around the exact write, not by guessing. Fixed
by having the skip path call `ensureTable()` too, same as the real-embed
path (cheap — a no-op once dimensions/model already match, so this costs
nothing on every subsequent save). `tests/unit/IndexUpdaterEmbeddingTest.cpp`
has a permanent regression test processing a too-short document first, then
a real one, and asserting the short document's state survives.

### A fifth root cause, found re-checking the SAME bug on more real queries again: a distance threshold alone can't bound a MULTI-word query's blurriness either

A further round of live verification (real prod queries — "markdown backlinks
wiki", "unique_ptr shared_ptr raii", "peer to peer networking keys", among
others) still found too many results for several multi-word queries, all
with `snippetIsHighlighted: false`, even with `welcome.md` correctly excluded
by the fix above. First suspected as a repeat of the same "weak document
vector" root cause, or possibly a BM25 lexical false-positive (`FtsSearch.cpp`'s
`buildMatchExpression()` treats a multi-word query as AND-of-prefix-terms
across title/body/tags, not OR — ruled out by inspection, since none of the
leaking documents contained all query words as prefixes anywhere).

The actual cause only showed up after a local reproduction was made to
match the real document text BYTE FOR BYTE (title + `"\n\n"` + body, the
same concatenation `IndexUpdater::upsertOne()` embeds) — an earlier,
paraphrased/truncated local reconstruction (missing markdown tables, code
blocks, and even a `[[wiki-link]]` in one document) had measured several
genuinely unrelated documents ABOVE `max_distance` when their real, complete
text measured just under it:

| document | measured distance for "markdown backlinks wiki" |
|---|---|
| wiki-links-note-b (relevant) | 0.2329 |
| wiki-links-note-a (relevant) | 0.2851 |
| architecture-notes (relevant) | 0.3172 |
| tag-style-check-1 (marginal) | 0.3695 |
| smart-pointers (unrelated) | 0.4190 |
| wireguard (unrelated) | 0.4729 |
| systemd-timers (unrelated) | 0.4797 |
| asyncio (unrelated) | 0.4904 |
| move-semantics (unrelated) | 0.4925 |

All nine sit under `max_distance` (0.5) — a genuine, reproducible property of
this query against this vault on this model, not a filtering bug. A
three-word query whose words point at three fairly independent concepts
("markdown", "backlinks", "wiki") produces an embedding that's an averaged,
"blurry" point roughly equidistant from many topically unrelated documents at
once, on a small (23-document) vault with a small model. No single global
`max_distance` value serves both this query shape and a single, sharp
technical term like "stabilize" well at the same time — tightening the
threshold enough to exclude `move-semantics` here would risk re-excluding
genuinely relevant results for other, sharper queries (the same
tightening-a-shared-threshold risk already noted for `welcome.md` above).

**The fix**: cap the semantic candidate list by COUNT, not just distance.
`FtsSearch::tryHybridSearch()` now stops admitting neighbors into the
semantic candidate list once `embeddings.semantic_top_k` (default `5`) of
them have passed the distance cutoff — `EmbeddingIndexer::nearest()` is
documented to return neighbors already sorted nearest-first, so this keeps
exactly the closest ones. This targets the actual defect directly: RRF (see
`reciprocalRankFusion()`'s own comment) has no way to reject a candidate once
it's admitted to a ranked list, only rank it, so bounding admission is the
only place a fix like this can live — a looser rank-fusion weighting scheme
wouldn't help, since the leak happens before fusion ever sees these rowids.
`tests/unit/FtsSearchHybridTest.cpp` has a permanent regression test: five
documents, a query with zero lexical overlap with any of them (isolating the
semantic side, same technique the "stabilize" test uses), `max_distance` set
wide open (so distance alone would admit all five), and asserts exactly
`semantic_top_k` results come back with the one genuinely relevant document
among them.

## Query embedding latency, and caching it

Real, measured cost of a single `embedQuery()` call on the actual production
armv7 SBC (bge-small-en-v1.5, no GPU), timed directly on the real hardware
(not `qemu-arm-static` — real timing needs real silicon), one call per query
after a warm-up call to exclude one-time graph-setup cost:

| query | embedQuery() time |
|---|---|
| stabilize | 1366 ms |
| beet soup | 1465 ms |
| markdown backlinks wiki | 1513 ms |
| unique_ptr shared_ptr raii | 1518 ms |
| peer to peer networking keys | 2617 ms |

This dwarfs everything else in a hybrid search request — FTS5's own MATCH
query and the RRF merge are noise by comparison. Real end-to-end
`GET /api/search` latency measured over HTTP on the same box lines up
exactly: 1-1.7s per request, one as high as 4s.

**The fix isn't a faster model or a smaller one — it's not re-computing an
embedding for text already embedded once.** `static/js/pages/search.js`'s
tag/type filter checkboxes deliberately re-run a search with the SAME query
text on every toggle, no debounce (see that file's own comment) — a real,
common interaction, not a hypothetical one: type a query, wait out the
1-2.6s, get results, tick a tag filter, wait out the SAME 1-2.6s again for
text that hasn't changed at all.

`FtsSearch` now caches `embedQuery()` results keyed on the raw query text
(`queryEmbeddingCache_`, `FtsSearch.h`). Correct without tracking model
identity or `query_prefix` in the cache key: both are fixed for the whole
lifetime of the one `FtsSearch` instance `main.cpp` constructs at startup —
config.toml is only read once, at startup, so a model swap requires a
restart, which recreates this object and its cache from nothing. No
cross-request staleness is possible.

A hard cap (256 entries), not a real LRU: `/api/search` doesn't require
authentication for public content, so an unbounded map keyed on
caller-supplied query text would be a real memory-growth vector on this
project's resource-constrained target (2GB RAM, measured on the real box).
Clearing the whole cache on overflow rather than evicting one entry at a
time is deliberately simple — personal-wiki query volume doesn't come close
to needing real LRU bookkeeping, and an occasional full flush costs one
more slow `embedQuery()` call, not a correctness problem.

The cache lookup and the `embedQuery()` call are under TWO SEPARATE lock
acquisitions, not one held across both — `embedQuery()` alone costs
1.3-2.6s, and holding the cache's mutex across it would serialize every
concurrent search behind whichever request happens to be computing an
embedding, even for entirely unrelated query text. `LocalEmbeddingProvider`
already has its own `embedMutex_` for `embed()`/`embedQuery()` itself (see
"A fifth real bug" above) — this cache adds nothing to that existing
serialization, it just lets a cache HIT skip the wait entirely.
`tests/unit/FtsSearchHybridTest.cpp` has a permanent regression test using
a counting embedding-provider wrapper: asserts a second identical query
makes zero additional `embed()` calls, and a genuinely different query
still makes one.

## Why two provider kinds, not one

A personal wiki has fail-safe-private documents by default (see `architecture.md`). A
single mandated embedding source is wrong for this project either way:

- **Cloud API** (OpenAI/Gemini/Voyage-style embeddings endpoint) is the cheap, fast
  path to a working feature, but it means every indexed document's content leaves the
  machine to a third party — a real, material privacy regression for a tool whose whole
  premise is "your own private notes, self-hosted." Not acceptable as the *only* option,
  but reasonable as an explicit, opt-in one for someone who doesn't keep sensitive
  content in their vault or who already trusts a given provider.
- **Local inference** (a small quantized model run in-process) keeps every document on
  the machine, matching the project's existing privacy posture. The cost is a heavier
  binary and a real, now-confirmed-nontrivial cross-compilation story for the ARM/SBC
  deployment path this project explicitly supports (see `deployment.md`,
  `sbc-deployment.md`).

Neither is strictly better, so both are supported as **build-time-optional, mutually
independent** features, selectable again at **runtime** via config — see the matrix
below. A build can carry zero, one, or both compiled in; a running binary can then be
configured to actually use zero, one, or (in principle, later) both.

## Build-time × runtime matrix

| Compiled in | `[embeddings].provider =` | Result |
|---|---|---|
| neither | `"none"` (default) | FTS5 only. Always valid regardless of build. |
| neither | `"local"` or `"cloud"` | **Startup error, not silent fallback** — see "Fail loudly" below. |
| local only | `"local"` | Local inference used. |
| local only | `"cloud"` | Startup error (cloud support not compiled in). |
| cloud only | `"cloud"` | Cloud API used. |
| both | `"local"` or `"cloud"` | Either, selected by config, no rebuild needed. |

### Fail loudly, not silently

A binary asked (via `config.toml`) to use a provider it wasn't built with must refuse
to start with a clear error naming the missing `WIKI_ENABLE_*` flag — never silently
degrade to `"none"`. A silent fallback here is exactly the failure shape
`~/.claude/CLAUDE.md`'s global "don't let a helper's stderr become a silent wrong
answer" rule warns about: an admin who set `provider = "local"` and restarted would see
search still work (FTS5 always does) and have no signal that semantic search quietly
never turned on at all.

## Build-time options

```
-DWIKI_ENABLE_LOCAL_EMBEDDINGS=ON   # links llama.cpp (FetchContent, pinned commit)
-DWIKI_ENABLE_CLOUD_EMBEDDINGS=ON   # HTTP client only — no new heavy dependency
```

Both default **OFF**. An SBC build stays as light as today's build until someone
opts in; a dev-machine build can carry both. Mirrors the existing
`WIKI_ENABLE_FUZZING` pattern (`CMakeLists.txt`).

`WIKI_ENABLE_CLOUD_EMBEDDINGS` is cheap — the project already links `httplib`
transitively via cpp-mcp's vendored copy for outbound HTTP, so a cloud provider adds
no new heavy dependency, just a small client class.

`WIKI_ENABLE_LOCAL_EMBEDDINGS` pulls in `llama.cpp` via `FetchContent`, the same
mechanism and pinned-commit discipline already used for `cpp-mcp` (see
`CMakeLists.txt`'s existing comment on why — no vcpkg port, spiked clean before
committing).

## The real zig/arm-musl bug found cross-compiling llama.cpp

Before committing to `llama.cpp` as the local-inference engine, it was spiked against
this project's actual `cross/arm-musl` toolchain (the same `zig cc`/`zig c++` wrapper
already used to build and deploy `wiki-server`/`wiki-mcp` to real armv7 hardware — see
`deployment.md`'s cross-compilation section) — not just built natively and assumed to
cross-compile. It didn't, on the first attempt, and the failure was a real toolchain
bug, not anything wrong in llama.cpp itself:

**`zig 0.16.0`'s ARM `-march=` parser rejects any LLVM-standard `-march=<arch>-a`
string outright** — confirmed with a minimal, llama.cpp-independent repro:
`zig cc -target arm-linux-musleabihf -march=armv7-a -c test.c` fails with
`error: unknown CPU: 'armv7'` even with **zero** feature extensions appended. The
dash-suffixed `-a` form (the standard LLVM/GCC spelling) is what breaks; zig's own
non-standard spelling (`-mcpu=generic+v7a`, no dash) — which `cross/arm-musl/cc`
already uses as its default — works fine. This is the same toolchain that already
has one confirmed, unrelated real bug documented in `CLAUDE.md` (`lld` SIGSEGV on a
linker depfile flag for this exact target) — a second, independent bug in the same
toolchain/target combination, not a pattern that should cast doubt on the first
finding or vice versa; the two are unrelated code paths (link-time vs. `-march`
parsing).

ggml's own `CMakeLists.txt` (`ggml/src/ggml-cpu/CMakeLists.txt`) only emits a
`-march=` flag at all when `GGML_CPU_ARM_ARCH` is explicitly set — leaving it unset
avoids the broken code path entirely, falling back to whatever `-mcpu` the toolchain
wrapper already supplies (`generic+v7a`). That alone under-builds one file
(`ggml-cpu/llamafile/sgemm.cpp`) which needs ARM NEON FP16 load intrinsics
(`vld1q_f16`/`vld1_f16`) not available at `generic+v7a` — feature-detection-failed
output during configure already reported `fp16`/`neon` support as absent at that
baseline. Passing `-mcpu=generic+v7a+fp16` a second time via `CMAKE_C_FLAGS`/
`CMAKE_CXX_FLAGS` (clang honors the *last* `-mcpu` on the command line, so this
cleanly overrides the wrapper's own default without editing the wrapper itself)
fixed it — verified by a full rebuild: 73/73 targets, `libllama.a` produced as a
real statically-linked `ELF 32-bit LSB relocatable, ARM, EABI5` archive.

**`+fp16` is safe for this project's actual deployment target** — `toolchain.cmake`'s
own header comment already documents the target as "Cortex-A7-class, hardfloat"
(Raspberry Pi 2-class hardware), and Cortex-A7 has VFPv4 with FP16 conversion — this
isn't reaching for a feature the real hardware lacks.

**This override is scoped narrowly, not applied globally.** `cross/arm-musl/cc`/`c++`
are NOT being edited to add `+fp16` to their own default `-mcpu` — that would silently
change codegen for every existing target (`wiki-server`, `wiki-mcp`, every overlay
port) for a feature only the new, optional local-embeddings path needs. Instead,
`CMAKE_C_FLAGS`/`CMAKE_CXX_FLAGS` are overridden only around the `llama.cpp`
`FetchContent_MakeAvailable()` call — see the CMake skeleton section below — leaving
every other target's compile flags untouched.

## `EmbeddingProvider` abstraction

`src/embeddings/EmbeddingProvider.h` — a minimal interface, matching this project's
established discipline of hiding a vendored SDK behind a small internal abstraction
(the same shape as `McpToolRegistry` hiding cpp-mcp): `embed(text) -> vector<float>`
plus `dimensions()`, `modelIdentifier()`, and `embedQuery(text)` (default =
`embed(text)`, overridable for an asymmetric retrieval model that expects the
query and passage sides embedded differently — see "A real relevance bug"
above for why this exists and what it fixed). Nothing else — no speculative
batching/streaming API added ahead of an actual second caller needing it.

`src/embeddings/EmbeddingProviderFactory.*` reads `AppConfig.embeddings` and returns
the right implementation, or throws a clear `std::runtime_error` naming the missing
build flag per the "fail loudly" rule above. `NullEmbeddingProvider` (provider =
`"none"`) is always compiled in and always available — it's not behind either
`WIKI_ENABLE_*` flag, since `"none"` must always be a valid, always-buildable choice.

## Runtime config (`config.toml`)

```toml
[embeddings]
provider = "none"        # "none" | "local" | "cloud"
# model_path = "..."     # local only — path to a GGUF model file
# api_key_env = "..."    # cloud only — name of an env var holding the API key,
                          # never the raw key itself (see config.example.toml)
# query_prefix = "..."   # local only — prepended to search queries only, not
                          # documents (see "A real relevance bug" above)
# max_distance = 0.5     # cosine-distance cutoff for a semantic match — default
                          # shown even if commented out (see same section)
# min_content_words = 6  # skip embedding entirely below this many words — default
                          # shown even if commented out (see same section)
# semantic_top_k = 5      # hard cap on semantic candidates, on TOP of max_distance —
                          # default shown even if commented out (see "A fifth root
                          # cause" above)
```

## Phased rollout

1. **Skeleton (done)** — build-time options, `EmbeddingProvider` interface,
   `NullEmbeddingProvider`, factory with fail-loudly behavior, `[embeddings]` config
   parsing, tests.
2. **Local provider (done)** — `LocalEmbeddingProvider` wired to `llama.cpp`'s embedding
   API, verified against a real `bge-small-en-v1.5` GGUF model with real
   cosine-similarity numbers (see "Real-model verification" above) and cross-compiled
   for the `cross/arm-musl` target (verified build only so far — not yet run on real
   ARM hardware the way `wiki-server`/`wiki-mcp` themselves have been; that's still
   open). Reordered ahead of the cloud provider from the original plan once the user
   picked local first specifically to get something real to verify offline, with no
   external API dependency.
3. **Cloud provider (done)** — `CloudEmbeddingProvider` (OpenAI `text-embedding-3-small`,
   1536-dim), verified against the real API (see "Real-API verification" above). No new
   heavy dependency — reuses cpp-mcp's own vendored `httplib.h`.
4. **`sqlite-vec` storage (done)** — `EmbeddingIndexer` (`document_embeddings` via
   `vec0`), wired into `IndexUpdater`'s write path and `main.cpp`'s wiring (including
   `VaultWatcher`'s own `IndexUpdater`), live-verified end to end against a real
   running server (see "Live end-to-end verification" above). `wiki-mcp`'s write
   tools now embed too, lazily — see "Closed gap" above.
5. **Hybrid ranking (done)** — `FtsSearch` blends BM25 + cosine ranking via
   Reciprocal Rank Fusion, live-verified over real HTTP (see "Hybrid ranking — live
   end-to-end verification" above). `wiki-mcp`'s `search_documents` tool stays
   FTS5-only for now — a deliberate scope boundary, not the same gap as the write
   tools (which is now closed) — see "Closed gap" above for why it's a different
   shape of problem.
6. **`LocalEmbeddingProvider` on real ARM hardware (done)** — cross-compiled with the
   same `cross/arm-musl` toolchain and march fix as `wiki-server`/`wiki-mcp`, copied to
   the real production SBC's `/tmp` (never touching `/opt/wiki` — no live service or
   data affected), and run there directly over SSH. Real result, on real armv7l
   hardware, not just a successful cross-compile: `dimensions() == 384`,
   `cosine(cat, kitten) = 0.781`, `cosine(cat, car) = 0.437`,
   `cosine(cat, identical) = 1.0` — matching the x86_64 numbers in "Real-model
   verification" above to 5 decimal places (the tiny remaining difference is
   expected floating-point variance between architectures, not a bug). Copied files
   removed from the SBC afterward.
7. **Deployed to real production (done)** — `embeddings.provider = "local"` is now
   actually live on the real deployment target (see
   `~/.claude/projects/-home-slayer-devel-wiki/memory/wiki-prod-deployment-target.md`
   for the real host — deliberately not named here, same reasoning as
   `deployment.md`'s own placeholder host). See "Deployed to production" below for
   the full process and the one real operational cost this surfaced (slower
   restarts).

Each step lands with its own tests and its own live verification before the next
starts — not built speculatively ahead of the step that actually needs it.

## Deployed to production

`embeddings.provider = "local"` is live on the real production instance (see
`docs/deployment.md`'s "Recording your own live deployment target" for where the
real host is actually recorded — never in this file, same reasoning as that section
gives). Process: cross-compiled `wiki-server`/`wiki-mcp` with
`-DWIKI_ENABLE_LOCAL_EMBEDDINGS=ON` using the existing `cross/arm-musl` toolchain and
`vcpkg_installed_arm` prefix (see `deployment.md`'s "Cross-compilation" section — no
new process, just the existing one plus one CMake flag), verified under
`qemu-arm-static` (`unit_tests` — 368/368 assertions — and a `--create-admin` smoke
test) before shipping anything to real hardware, per that section's own discipline.
Deployed with the project's established backup-before-replace pattern: old
`wiki-server`/`wiki-mcp` binaries and `config.toml` copied aside with a timestamp
suffix before the new ones landed, the model placed at a new, permanent
`/opt/wiki/models/` (not `/tmp` — this copy is meant to stay), `config.toml` gained
an appended `[embeddings]` block, then `systemctl restart wiki.service`.

**Real operational cost this surfaced**: `main.cpp`'s startup sequence runs
`IndexBuilder::fullRescan()` (a full vault walk + reindex) unconditionally on every
process start, already true before this change — but every one of those reindexed
documents now ALSO gets a real embedding computed, synchronously, before
`wiki-server` starts listening on its HTTP port at all. For this instance's 23
real documents, that added roughly 30 seconds to every `systemctl restart` (`nginx`
briefly served `502` for callers hitting it during that window) — not a bug, and not
something a small personal vault is likely to notice in practice, but a genuinely
slower restart than before this change, worth knowing about before enabling this on
an instance with hundreds of documents rather than dozens, or before restarting
during a moment someone might actually be trying to use the site.

**Verified against real content, not a test fixture**: `GET /api/search?q=beet+soup`
(zero words in common with the stored recipe) returned `recipes/dinner/borscht.md`
first, `snippetIsHighlighted: false` confirming it came through the semantic path —
the exact same hybrid-search proof from earlier in this document, now demonstrated
against this project's own real wiki content instead of a synthetic test fixture.
Process RSS after startup: ~107 MiB (model + inference buffers) on a 2 GiB device,
1.6 GiB still reported available — not tight, but worth knowing for anyone
provisioning a smaller board.

Old binaries/config left in place as timestamped backups (`wiki-server.bak-<ts>`,
`wiki-mcp.bak-<ts>`, `config.toml.bak-<ts>`) — not cleaned up automatically, same
"don't delete what might be a rollback path" caution as everywhere else in this
project's operational discipline.
