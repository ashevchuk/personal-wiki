# Semantic search (embeddings)

Semantic search blends FTS5 BM25 lexical ranking with cosine-distance vector
ranking (`sqlite-vec`), combined via Reciprocal Rank Fusion. It is **additive to
FTS5, never a replacement** — with `[embeddings].provider = "none"` (the default),
FTS5 is the only search path, on every build.

Two provider kinds exist because a single mandated embedding source is wrong for a
project whose documents are fail-safe-private by default:

- **Cloud** (any OpenAI-compatible `POST {api_base}/embeddings`) is the cheap, fast
  path, but every indexed document's content leaves the machine to whatever host
  `api_base` names. Reasonable as an explicit opt-in — against a loopback server
  (Ollama, LM Studio) nothing leaves the box at all; against `api.openai.com` it does.
- **Local** (a small quantized model run in-process via llama.cpp) keeps every
  document on the machine, at the cost of a heavier binary and a real
  cross-compilation story for the ARM/SBC deployment path (see `deployment.md`,
  `sbc-deployment.md`).

Both are build-time-optional and mutually independent; a running binary is then
configured at runtime to use zero, one, or (in principle) either.

## Build-time options

```
-DWIKI_ENABLE_LOCAL_EMBEDDINGS=ON   # links llama.cpp (FetchContent, pinned commit)
-DWIKI_ENABLE_CLOUD_EMBEDDINGS=ON   # HTTP client only — no new heavy dependency
```

Both default **OFF** — an SBC build stays as light as today's build until someone
opts in. `WIKI_ENABLE_CLOUD_EMBEDDINGS` adds no new dependency (the project already
links `httplib` transitively via cpp-mcp's vendored copy). `WIKI_ENABLE_LOCAL_EMBEDDINGS`
pulls in `llama.cpp` via `FetchContent`, the same pinned-commit mechanism already used
for `cpp-mcp`.

### Build-time × runtime matrix

| Compiled in | `[embeddings].provider =` | Result |
|---|---|---|
| neither | `"none"` (default) | FTS5 only. Always valid regardless of build. |
| neither | `"local"` or `"cloud"` | Startup error — refuses to run rather than silently falling back to `"none"`. |
| local only | `"local"` | Local inference used. |
| local only | `"cloud"` | Startup error (cloud support not compiled in). |
| cloud only | `"cloud"` | Cloud API used. |
| both | `"local"` or `"cloud"` | Either, selected by config, no rebuild needed. |

A binary asked to use a provider it wasn't built with always refuses to start with an
error naming the missing `WIKI_ENABLE_*` flag — it never silently degrades to `"none"`,
since that would leave FTS5 still working with no signal that semantic search never
turned on.

### Native builds and `GGML_NATIVE`

Native (non-cross-compiling) builds must leave ggml's CPU feature detection on
(`GGML_NATIVE ON`), not hardcode it off — hardcoding it off pins one fixed CPU backend
baked in at configure time (e.g. `-mavx2`) regardless of what the actual build host
supports, which crashes with `SIGILL` on a host lacking that feature (such as AVX/FMA/
F16C without AVX2) even though the build itself succeeds. `GGML_NATIVE` is forced
`OFF` only when `CMAKE_CROSSCOMPILING` is set, since ggml must never probe the build
host's CPU when the run target is a different architecture.

### Cross-compiling llama.cpp for ARM (`cross/arm-musl`)

zig 0.16.0's ARM `-march=` parser rejects the standard LLVM/GCC `-march=<arch>-a`
spelling outright; zig's own non-standard spelling (`-mcpu=generic+v7a`, no dash,
already the `cross/arm-musl` wrapper's default) works. Leaving `GGML_CPU_ARM_ARCH`
unset avoids the broken path but then under-builds `ggml-cpu/llamafile/sgemm.cpp`,
which needs ARM NEON FP16 intrinsics not available at `generic+v7a`. The fix is to
pass `-mcpu=generic+v7a+fp16` a second time via `CMAKE_C_FLAGS`/`CMAKE_CXX_FLAGS`
(clang honors the last `-mcpu` on the command line), scoped narrowly to just the
llama.cpp `FetchContent_MakeAvailable()` call in `CMakeLists.txt` — not applied to
`cross/arm-musl/cc`/`c++`'s own defaults, so every other target's codegen stays
unaffected. `+fp16` is safe for this project's deployment target: Cortex-A7-class
hardware has VFPv4 with FP16 conversion.

This is an independent bug from the `lld` SIGSEGV already documented for this same
toolchain/target in `CLAUDE.md` — unrelated code paths (link-time vs. `-march`
parsing).

## `EmbeddingProvider` abstraction

`src/embeddings/EmbeddingProvider.h` — a minimal interface, matching this project's
pattern of hiding a vendored SDK behind a small internal abstraction:
`embed(text) -> vector<float>`, `dimensions()`, `modelIdentifier()`, and
`embedQuery(text)` (defaults to `embed(text)`; overridable for an asymmetric
retrieval model that expects the query and passage sides embedded differently — see
"Relevance tuning" below). `src/embeddings/EmbeddingProviderFactory.*` reads
`AppConfig.embeddings` and returns the right implementation, or throws per the
fail-loudly rule above. `NullEmbeddingProvider` (`provider = "none"`) is always
compiled in regardless of either `WIKI_ENABLE_*` flag, since `"none"` must always be a
valid choice.

## Runtime config (`config.toml`)

```toml
[embeddings]
provider = "none"        # "none" | "local" | "cloud"
# model_path = "..."     # local only — path to a GGUF model file
# api_key_env = "..."    # cloud only — NAME of an env var holding the API key,
                          # never the raw key itself (see config.example.toml);
                          # optional for a local server that doesn't authenticate
# api_base = "..."       # cloud only — OpenAI-compatible root including /v1
                          # (default https://api.openai.com/v1). POST {api_base}/embeddings
# model = "..."          # cloud only — JSON "model" field (default
                          # text-embedding-3-small)
# dimensions = 1536      # cloud only — vector width; sqlite-vec needs this
                          # before any HTTP call (default 1536). MUST match the
                          # model; a mismatch fails embed() rather than truncating
# query_prefix = "..."   # local only — prepended to search queries only, not
                          # documents (see "Relevance tuning" below)
# max_distance = 0.5     # cosine-distance cutoff for a semantic match — default
                          # shown even if commented out
# min_content_words = 6  # skip embedding entirely below this many words — default
                          # shown even if commented out
# semantic_top_k = 5      # hard cap on semantic candidates, on top of max_distance —
                          # default shown even if commented out
```

### OpenAI-compatible cloud endpoints

`provider = "cloud"` is not locked to OpenAI. `CloudEmbeddingProvider` POSTs
`{"model": "...", "input": "..."}` to `{api_base}/embeddings`, optionally with
`Authorization: Bearer`, and reads `data[0].embedding` as a float array — the OpenAI
Embeddings API shape. Any server that speaks that JSON (Ollama's OpenAI-compatibility
layer, LM Studio, Together, Fireworks, vLLM, a local proxy) is a valid target. The
binary must be built with `-DWIKI_ENABLE_CLOUD_EMBEDDINGS=ON`; a `provider = "cloud"`
on a binary that wasn't fails at startup.

**Defaults** (leave the knobs unset and you get OpenAI itself):

| Knob | Default | Meaning |
|---|---|---|
| `api_base` | `https://api.openai.com/v1` | Absolute `http(s)` URL. Trailing slashes stripped. The request path is `{api_base}/embeddings`, so the `/v1` prefix belongs **in** `api_base` for any server that uses it. |
| `model` | `text-embedding-3-small` | JSON `"model"` field, sent as-is. |
| `dimensions` | `1536` | Vector width. sqlite-vec needs this at `CREATE` time, **before** any HTTP call, so it cannot be inferred from the first response. `0` in config means this default. A response whose length doesn't match fails `embed()` with a message to set `embeddings.dimensions` — it does not truncate. |
| `api_key_env` | unset | The **name** of an environment variable holding the API key, never the key itself (`config.toml` is plain text). Unset = no `Authorization` header (local servers that don't authenticate). If a name **is** set, that variable must be present and non-empty at process start or construction throws. |

`modelIdentifier()` is `cloud:{api_base}:{model}`, not just the model name — switching
endpoint or model drops and recreates `document_embeddings` even when the width stays
the same, since two same-width models are not the same vector space.

**Not this API.** Azure OpenAI (deployment-shaped URLs plus `api-version=`), Gemini's
native embeddings endpoint, Voyage, and anything that isn't
`POST {api_base}/embeddings` with that JSON body. Pointing `api_base` at those will
not work.

**Where the key lives.** systemd's `EnvironmentFile=-/etc/opt/wiki/wiki.env` (see
`systemd/wiki.env.example`) is the documented place for
`WIKI_EMBEDDINGS_API_KEY=...` — or whatever name `api_key_env` points at.
`CloudEmbeddingProvider` reads it via `getenv`, not via `AppConfig`.

Examples:

```toml
# OpenAI (same as leaving api_base/model/dimensions commented out)
[embeddings]
provider = "cloud"
api_key_env = "WIKI_EMBEDDINGS_API_KEY"

# Ollama on the same machine (no key)
[embeddings]
provider = "cloud"
api_base = "http://127.0.0.1:11434/v1"
model = "nomic-embed-text"
dimensions = 768

# LM Studio
[embeddings]
provider = "cloud"
api_base = "http://127.0.0.1:1234/v1"
model = "text-embedding-nomic-embed-text-v1.5"
dimensions = 768
```

Privacy depends on **which host** `api_base` names, not on the word `"cloud"` in
config — Ollama on loopback never leaves the machine; `api.openai.com` sends every
indexed document's title+body to a third party.

### Testing against a real local model

Not run by default (no GGUF model is vendored). Opt in with both flags:

```sh
cmake -S . -B build -DWIKI_ENABLE_LOCAL_EMBEDDINGS=ON \
  -DWIKI_TEST_EMBEDDING_MODEL_PATH=/path/to/bge-small-en-v1.5-f16.gguf
cmake --build build --target local_embedding_provider_test
ctest --test-dir build -R local_embedding_provider_test --output-on-failure
```

### Testing against the real cloud API

Gated at build time (`WIKI_ENABLE_CLOUD_EMBEDDINGS`) and skipped at runtime (Catch2
`SKIP`) when `OPENAI_API_KEY` isn't present in the environment:

```sh
cmake -S . -B build -DWIKI_ENABLE_CLOUD_EMBEDDINGS=ON
cmake --build build --target cloud_embedding_provider_test
set -a && source .env && set +a   # or otherwise export a real OPENAI_API_KEY
ctest --test-dir build -R cloud_embedding_provider_test --output-on-failure
```

## Storage (`sqlite-vec`)

`EmbeddingIndexer` (`src/index/EmbeddingIndexer.{h,cpp}`) owns the `sqlite-vec`-backed
`document_embeddings` virtual table — `ensureTable(dimensions, modelIdentifier)`,
`upsertOne`/`removeOne`, and `nearest()` (KNN via `vec0`'s own `MATCH`/`ORDER BY
distance`). Vendored via `FetchContent` + pinned commit, compiled directly into
`wikicore`, and registered once per process via `sqlite3_auto_extension()` in
`Database.cpp` — that file declares `sqlite3_vec_init`'s prototype by hand rather than
including `sqlite-vec.h` directly, because that header pulls in `sqlite3ext.h`'s
macro-based `sqlite3_api` diversion table (meant for the loadable-extension file
itself) and silently breaks every ordinary `sqlite3_*` call elsewhere in the same
translation unit.

`IndexUpdater::upsertOne`/`removeOne` call into `EmbeddingIndexer` automatically
whenever an `EmbeddingProvider*` is passed to their constructor (optional, defaults to
`nullptr`). `main.cpp` constructs the configured provider once at startup and passes
it to every `IndexUpdater` instance, including `VaultWatcher`'s own, so a document
changed by an external editor is embedded too.

**`vec0`'s vector width is fixed at `CREATE` time** — `CREATE VIRTUAL TABLE ... USING
vec0(embedding float[N])` takes `N` as a DDL literal, not a bind parameter, so one
table can't hold both a 384-dim and a 1536-dim provider's vectors.
`EmbeddingIndexer::ensureTable()` tracks the table's current width (and the
provider's `modelIdentifier()`) in `index_meta`, and drops and recreates the table
empty on a mismatch — an accepted cost, since the whole index is already documented
project-wide as disposable and rebuildable from the vault.

### Chunking

A document is split into overlapping passage windows rather than embedded as one
mean-pooled vector of its whole title+body — a long note otherwise drowns a relevant
paragraph in the average, and a model with a small context window (512 tokens for
`bge-small-en-v1.5`) would reject the whole document outright once the concatenated
text tokenizes past it. `embeddings::chunkForEmbedding` produces ~384-estimated-token
windows with 64-token overlap (cap 32 chunks), each chunk prefixed with the document
title; `EmbeddingIndexer` stores one `vec0` row per chunk sharing `document_rowid`,
and `nearest()` collapses chunks back to unique documents at the best (minimum) chunk
distance before RRF sees them. A short note still produces a single
`title + "\n\n" + body` chunk. Token counts are estimated (2 UTF-8 bytes/token), not
computed with the model's real tokenizer — `EmbeddingProvider` stays
tokenizer-agnostic, and an oversized chunk still fails as a catchable `embed()` error
(see "Oversized documents" below) rather than a crash.

Embedding on save runs on a background worker — the HTTP Save returns as soon as the
markdown and FTS5 index are committed, not after embedding finishes. The embed worker
coalesces by document: a second Save of the same file replaces any still-waiting job,
and an identical body already queued or in-flight is dropped rather than embedded
twice. `--reindex` / `POST /api/admin/reindex` wait for embeddings to finish
(`IndexUpdater::flushEmbeddings()`).

### Oversized documents

`LocalEmbeddingProvider::embed()` checks the tokenized length against `maxTokens_`
(the model's own `n_ctx`, captured at construction) and throws a plain
`std::runtime_error` before calling `llama_decode()` if it's exceeded — llama.cpp's
own internal assert on an over-length input calls `abort()` directly, with no C++
exception for a normal `catch` to intercept. A text longer than the model's trained
context window also produces a semantically meaningless embedding even when it
doesn't crash, so rejecting it outright is the correct behavior, not merely the safe
one. Chunking (above) means a long note should normally succeed as several in-window
passages instead of ever reaching this path; the check remains load-bearing for any
caller that still calls `embed()` directly (an admin retry, a future caller) and as a
backstop if the estimate-based splitter ever packs a chunk the real tokenizer still
finds too long.

### Concurrency

`provider_->embed()` always runs with no lock held — a slow embed (network call or
model inference) must never block other threads' document saves.
`LocalEmbeddingProvider` serializes its own `embed()`/`embedQuery()` calls internally
(`embedMutex_`), since `llama_context`'s mutable KV-cache state isn't reentrant across
threads sharing one provider instance. `CloudEmbeddingProvider` needs no such lock —
it constructs a fresh HTTP client per call. Every actual database write this feature
makes (document/FTS transactions and `EmbeddingIndexer`'s own `ensureTable()`/
upsert transactions alike) shares the single `IndexUpdater::mutex_` — two independent
mutexes guarding the same underlying `sqlite3` connection is not safe, regardless of
which part of the write path each one guards.

## Incremental re-embedding

`document_embedding_state` (`src/index/schema.h`'s `kMigration5`) tracks, per
document, the `content_hash` (`std::hash` of `title + "\n\n" + body`) as of its last
*successful* embed, and `last_error` (`NULL` exactly when that attempt succeeded).
`IndexUpdater::upsertOne` computes the current hash and only queues a real `embed()`
call when `EmbeddingIndexer::needsEmbedding()` reports a mismatch — this is what keeps
a routine restart with no content changes fast.

**A failed embed attempt never touches `content_hash`, only `last_error`.** An old,
still-accurate vector for unchanged content isn't invalidated by an unrelated later
failure, and a document whose embed attempt failed stays permanently mismatched
against its last-successful hash, so the next rescan or save retries automatically —
no separate pending-retry queue.

**A model swap forces a full re-embed even at unchanged dimensionality.** Two
different models can both produce 384-dim vectors while encoding incompatible vector
spaces, so a dimension match alone can't prove compatibility.
`EmbeddingProvider::modelIdentifier()` (`"local:<path>:<mtime>:<size>"`;
`"cloud:{api_base}:{model}"`) is compared alongside `dimensions()` in
`ensureTable()`; a mismatch on either drops and recreates `document_embeddings` and
clears every row of `document_embedding_state`.

**Documents below `embeddings.min_content_words` (default 6) skip embedding
entirely.** A document with almost no prose doesn't carry a strong enough signal to
land reliably on one side of any distance cutoff for every query — the skip is
recorded as success-with-no-vector (so it never appears in "needing attention"), and
removes any stale vector from a previously longer version of the same document. A
later edit that adds enough content re-triggers a real embed via the normal
content-hash mismatch.

## Hybrid ranking

`FtsSearch::search()` (text-search mode only) blends BM25 and cosine-distance ranking
via Reciprocal Rank Fusion: `bm25CandidateRowIds()` and `EmbeddingIndexer::nearest()`
each independently rank up to 200 candidates, and RRF (`score += 1/(60 + rank)` per
list, the standard `k=60` constant) merges them by rank position, with no need to
normalize BM25 scores and cosine distances onto a shared scale. `limit`/`offset`
apply to the merged ranking. A rowid found only via the semantic side falls back to
the document's stored excerpt instead of FTS5 `snippet()`, which has no defined
result outside a `MATCH` context.

Falls back to plain FTS5-only ranking whenever hybrid can't proceed: no provider
configured, no `document_embeddings` table yet, or `embed()` itself failing — same
best-effort, additive-never-load-bearing posture as the write path.

### Filters apply to both candidate sources

Tag/type/folder filters (`appendCommonFilterSql()`) apply identically to the BM25 and
semantic candidate sources — a semantic-only candidate that fails an active filter is
excluded the same way a BM25 candidate would be. Known limitation: filtering happens
after the merged RRF list is already sliced by `limit`/`offset`, so a filtered-out
semantic candidate isn't backfilled from further down the ranked list — an active
filter can return fewer than `limit` results even when more matches exist deeper in
the full ranking. Not a correctness problem (no wrong document is ever shown), just an
incompleteness not worth joining tag/type into the semantic query itself for a vault
this size.

### Relevance tuning

On a small vault, "the N closest neighbors" is effectively the whole vault, and RRF
has no way to reject an already-admitted candidate, only rank it — so three knobs
bound what reaches RRF in the first place:

- **`embeddings.query_prefix`** (local only, empty by default) — some retrieval models
  (e.g. `bge-small-en-v1.5`) are trained with an instruction prefix on the query side
  only (its own model card recommends `"Represent this sentence for searching
  relevant passages: "`). `EmbeddingProvider::embedQuery()` applies it;
  `FtsSearch::tryHybridSearch()` always calls `embedQuery()`, never `embed()`, for the
  query side. `CloudEmbeddingProvider` needs no prefix — OpenAI-compatible embeddings
  are symmetric.
- **`embeddings.max_distance`** (default `0.5`) — a cosine-distance cutoff applied to
  `nearest()`'s results before they reach RRF.
- **`embeddings.semantic_top_k`** (default `5`) — a hard cap on how many
  distance-cutoff-passing neighbors are admitted, taken in the nearest-first order
  `nearest()` already returns. A short, sharp query and a multi-concept query produce
  embeddings with very different "blurriness," so a single distance threshold alone
  cannot bound both; capping by count does.

### Query embedding caching

Computing a query's embedding is the dominant cost of a hybrid search — on the
project's reference ARM target, `embedQuery()` alone takes roughly 1.3-2.6s, dwarfing
the FTS5 MATCH query and RRF merge. `FtsSearch` caches `embedQuery()` results keyed on
the raw query text (`queryEmbeddingCache_`), since model identity and `query_prefix`
are both fixed for the lifetime of one `FtsSearch` instance (`config.toml` is only
read at startup). The cache is a hard 256-entry cap flushed wholesale on overflow, not
a real LRU — `/api/search` doesn't require authentication for public content, so an
unbounded map keyed on caller-supplied text would be a memory-growth vector, and
personal-wiki query volume doesn't justify real LRU bookkeeping. The cache lookup and
the `embedQuery()` call use separate lock acquisitions, so a cache miss computing one
query's embedding doesn't block concurrent lookups for other query text.

## Admin visibility and control

Two runtime-mutable settings, both SQLite-backed (no `config.toml`/restart involved),
plus an API surfacing the self-healing state above:

- **`EmbeddingsRuntimeConfig`** (`src/index/EmbeddingsRuntimeConfig.{h,cpp}`,
  `embeddings_runtime_config`) — a `vectorSearchEnabled` bool, read by
  `FtsSearch::search()` on every call (not just at construction), so an admin can
  disable hybrid ranking live, without restarting `wiki-server`.
- **`GET/POST /api/admin/embeddings-status`** (`src/controllers/AdminRoutes.cpp`,
  admin+CSRF, registered only on a `WIKI_ENABLE_SQLITE_VEC` build). `GET` returns
  whether a real provider is configured, its dimensions/model identity, the runtime
  toggle state, and `EmbeddingIndexer::listNeedingAttention()`. `PUT` flips the
  toggle. `POST .../reembed` re-derives one document's index row via
  `IndexBuilder::reindexOneFile` (not a full `--reindex`); `POST .../reembed-all` does
  that for every currently-listed document and reports how many still fail. Both are
  logged to `mcp_audit_log` (`"admin:reembed"`/`"admin:reembed-all"`), visible via
  `GET /api/admin/mcp-audit-log`. `reembed-all` writes one summary entry per call, not
  one per document.
- **Account page UI** (`static/js/pages/account.js`, "Semantic search (embeddings)"
  section) — a checkbox bound to the runtime toggle, and a "Documents needing
  attention" list with per-document and bulk Retry buttons. The section simply
  doesn't render on a `404` from `GET /api/admin/embeddings-status` (a build without
  embeddings compiled in).

`EmbeddingIndexer::listNeedingAttention()` returns every document with no state row
at all, or whose last attempt failed — deliberately excluding documents merely due for
a routine hash-mismatch re-embed on next save, since this list means "something needs
a human to look at it," not "this is due for normal work."

## Startup rescan

`IndexBuilder::fullRescan()` (unconditional at every `wiki-server` startup — see
`architecture.md`'s storage model) runs on a background thread rather than blocking
the HTTP listener; `main()` joins it after `drogon::app().run()` returns, so a
still-running rescan finishes cleanly on shutdown instead of racing process teardown.
The background rescan shares the same `IndexUpdater`/connection as request handlers,
serialized by the same mutex, so a document saved over HTTP while the rescan is still
walking the vault is indexed immediately by that save; search results fill in
incrementally rather than the whole site being unreachable until the rescan finishes.
`--reindex` / `POST /api/admin/reindex` stay fully synchronous, since a caller that
explicitly asks for a reindex wants to know when it's actually done.

`RescanProgress` (`src/index/RescanProgress.h`, all-atomic, no mutex needed) tracks
`inProgress`/`documentsIndexed`, cleared via an RAII guard so a rescan that throws
partway through doesn't leave the tracker stuck claiming "still running." `GET
/api/admin/reindex-status` (admin-only, no CSRF — a pure read of in-memory atomics)
exposes `{"inProgress":bool,"documentsIndexed":N}`; the account page polls it every
1.5s and shows a self-hiding status line while a rescan is in progress. Registered on
every build regardless of whether embeddings are compiled in, since a full-vault
rescan is relevant either way.

**Operational cost with local embeddings enabled.** Because every reindexed document
also gets a real embedding computed synchronously as part of the startup rescan,
restart time now scales with vault size under `provider = "local"` — on the order of
1-1.5 seconds per document on a constrained ARM SBC with no GPU. A vault of a few
dozen documents adds tens of seconds to every restart, during which `nginx` (or
whatever reverse proxy sits in front) serves `502` for anyone hitting the site. Worth
accounting for before enabling local embeddings on a large vault, or before
restarting during a moment someone might be using the site. Memory footprint is
dominated by the model itself (roughly 100 MiB resident for `bge-small-en-v1.5`).

## `wiki-mcp` write tools

`wiki-mcp` constructs an `IndexUpdater` for its `create_document`/`update_document`/
`attach_file` tools, but does not construct a real `EmbeddingProvider` at process
startup — `wiki-mcp` is spawned fresh per MCP session, and the overwhelmingly common
session never calls a write tool at all. `mcp/McpServer.cpp`'s `LazyEmbeddingProvider`
constructs the real provider on the first actual write call
(`IndexUpdater::setProvider()`); every write after the first reuses it. A failed
lazy-init (bad model path, missing API key) is logged to stderr once and the write
proceeds FTS5-only, the same best-effort posture as `wiki-server`'s own embedding
step.

`wiki-mcp`'s `search_documents` tool (and `FtsSearch` generally, when constructed
without a provider) stays FTS5-only on purpose — there's no equivalent "first write"
moment to lazily hook a provider into for a read-only tool, and wiring one in just for
reads would reopen the same per-session startup cost this lazy design avoids for
writes.
