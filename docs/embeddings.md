# Semantic search (embeddings)

Semantic search blends two ranking signals: FTS5's lexical BM25 ranking,
and cosine-distance ranking over vector embeddings (`sqlite-vec`),
combined via Reciprocal Rank Fusion. It's **additive to FTS5, never a
replacement** — with `[embeddings].provider = "none"` (the default), FTS5
is the only search path, on every build.

There are two ways to generate those embeddings, because mandating a
single source would be wrong for a project whose documents are
fail-safe-private by default:

- **Cloud** (any OpenAI-compatible `POST {api_base}/embeddings`) is the
  cheap, fast option, but every indexed document's content leaves the
  machine and goes to whatever host `api_base` names. That's fine as an
  explicit opt-in: against a loopback server (Ollama, LM Studio) nothing
  actually leaves the box; against `api.openai.com` it does.
- **Local** (a small quantized model run in-process via llama.cpp) keeps
  every document on the machine, at the cost of a heavier binary and a
  real cross-compilation story for the ARM/SBC deployment path (see
  `sbc-deployment.md`).

Both are build-time-optional and independent of each other. A running
binary is then configured at runtime to use zero, one, or in principle
either.

## Build-time options

```
-DWIKI_ENABLE_LOCAL_EMBEDDINGS=ON   # links llama.cpp (FetchContent, pinned commit)
-DWIKI_ENABLE_CLOUD_EMBEDDINGS=ON   # HTTP client only — no new heavy dependency
```

Both default to **OFF**, so an SBC build stays as light as it is today
unless someone opts in. `WIKI_ENABLE_CLOUD_EMBEDDINGS` adds no new
dependency — the project already links `httplib` transitively through
cpp-mcp's vendored copy. `WIKI_ENABLE_LOCAL_EMBEDDINGS` pulls in
`llama.cpp` via `FetchContent`, using the same pinned-commit mechanism
already used for `cpp-mcp`.

### What happens for each build/config combination

| Compiled in | `[embeddings].provider =` | Result |
|---|---|---|
| neither | `"none"` (default) | FTS5 only. Always valid, regardless of build. |
| neither | `"local"` or `"cloud"` | Startup error — refuses to run rather than silently falling back to `"none"`. |
| local only | `"local"` | Local inference used. |
| local only | `"cloud"` | Startup error (cloud support not compiled in). |
| cloud only | `"cloud"` | Cloud API used. |
| both | `"local"` or `"cloud"` | Either, selected by config, no rebuild needed. |

If a binary is asked to use a provider it wasn't built with, it always
refuses to start, with an error naming the missing `WIKI_ENABLE_*` flag.
It never silently falls back to `"none"` — that would leave FTS5 working
fine with no visible sign that semantic search never actually turned on.

### Native builds and `GGML_NATIVE`

A native (non-cross-compiling) build must leave ggml's CPU feature
detection on (`GGML_NATIVE ON`) rather than hardcoding it off. Hardcoding
it off pins one fixed CPU backend (e.g. `-mavx2`) at configure time
regardless of what the actual build host supports, which crashes with
`SIGILL` on a host lacking that feature — even though the build itself
succeeds. `GGML_NATIVE` is only forced `OFF` when `CMAKE_CROSSCOMPILING`
is set, since ggml must never probe the build host's CPU when the run
target is a different architecture.

### Cross-compiling llama.cpp for ARM (`cross/arm-musl`)

Three separate, narrow facts worth knowing if you touch this build:

- zig 0.16.0's ARM `-march=` parser rejects the standard LLVM/GCC
  `-march=<arch>-a` spelling outright. zig's own non-standard spelling
  (`-mcpu=generic+v7a`, no dash) works, and is already the
  `cross/arm-musl` wrapper's default.
- Leaving `GGML_CPU_ARM_ARCH` unset avoids that broken path, but then
  under-builds `ggml-cpu/llamafile/sgemm.cpp`, which needs ARM NEON FP16
  intrinsics not available at `generic+v7a`. The fix is to pass
  `-mcpu=generic+v7a+fp16` a second time via
  `CMAKE_C_FLAGS`/`CMAKE_CXX_FLAGS` (clang honors the last `-mcpu` on the
  command line), scoped narrowly to just the llama.cpp
  `FetchContent_MakeAvailable()` call in `CMakeLists.txt`. It's not
  applied to `cross/arm-musl/cc`/`c++`'s own defaults, so every other
  target's codegen stays unaffected. `+fp16` is safe for this project's
  deployment target: Cortex-A7-class hardware has VFPv4 with FP16
  conversion.
- This is a separate bug from the `lld` `SIGSEGV` already documented for
  this same toolchain/target in `CLAUDE.md` — the two involve unrelated
  code paths (link-time behavior vs. `-march` parsing).

## The `EmbeddingProvider` abstraction

`src/embeddings/EmbeddingProvider.h` is a minimal interface, matching
this project's usual pattern of hiding a vendored SDK behind a small
internal abstraction: `embed(text) -> vector<float>`, `dimensions()`,
`modelIdentifier()`, and `embedQuery(text)` (which defaults to
`embed(text)`, but can be overridden for a retrieval model that expects
the query and passage sides embedded differently — see "Relevance
tuning" below).

`src/embeddings/EmbeddingProviderFactory.*` reads `AppConfig.embeddings`
and returns the right implementation, or throws, following the
fail-loudly rule above. `NullEmbeddingProvider` (`provider = "none"`) is
always compiled in regardless of either `WIKI_ENABLE_*` flag, since
`"none"` must always be a valid choice.

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

`provider = "cloud"` isn't locked to OpenAI itself.
`CloudEmbeddingProvider` POSTs `{"model": "...", "input": "..."}` to
`{api_base}/embeddings`, optionally with `Authorization: Bearer`, and
reads `data[0].embedding` as a float array — that's the OpenAI Embeddings
API shape. Any server that speaks that same JSON (Ollama's
OpenAI-compatibility layer, LM Studio, Together, Fireworks, vLLM, a local
proxy) is a valid target. The binary must be built with
`-DWIKI_ENABLE_CLOUD_EMBEDDINGS=ON`; `provider = "cloud"` on a binary that
wasn't fails at startup.

**Defaults** (leave every knob unset and you get OpenAI itself):

| Knob | Default | Meaning |
|---|---|---|
| `api_base` | `https://api.openai.com/v1` | Absolute `http(s)` URL. Trailing slashes are stripped. The request path is `{api_base}/embeddings`, so the `/v1` prefix needs to be **in** `api_base` for any server that uses one. |
| `model` | `text-embedding-3-small` | JSON `"model"` field, sent as-is. |
| `dimensions` | `1536` | Vector width. sqlite-vec needs this at `CREATE` time, **before** any HTTP call, so it can't be inferred from the first response. `0` in config means this default. A response whose length doesn't match fails `embed()` with a message to set `embeddings.dimensions` — it never silently truncates. |
| `api_key_env` | unset | The **name** of an environment variable holding the API key, never the key itself (`config.toml` is plain text). Unset means no `Authorization` header is sent — fine for local servers that don't authenticate. If a name *is* set, that variable must exist and be non-empty at process start, or construction throws. |

`modelIdentifier()` is `cloud:{api_base}:{model}`, not just the bare model
name — switching the endpoint or the model drops and recreates
`document_embeddings`, even when the vector width stays the same, because
two same-width models don't necessarily share the same vector space.

**What this is not.** Azure OpenAI (which uses deployment-shaped URLs
plus `api-version=`), Gemini's native embeddings endpoint, Voyage, and
anything else that isn't `POST {api_base}/embeddings` with that exact
JSON body. Pointing `api_base` at any of those won't work.

**Where the key lives.** systemd's `EnvironmentFile=-/etc/opt/wiki/wiki.env`
(see `systemd/wiki.env.example`) is the documented place for
`WIKI_EMBEDDINGS_API_KEY=...`, or whatever name `api_key_env` points at.
`CloudEmbeddingProvider` reads it via `getenv`, never via `AppConfig`.

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

Privacy depends on **which host** `api_base` names, not on the word
`"cloud"` appearing in config — Ollama on loopback never leaves the
machine; `api.openai.com` sends every indexed document's title and body
to a third party.

### Testing against a real local model

This isn't run by default, since no GGUF model is vendored. Opt in with
both flags:

```sh
cmake -S . -B build -DWIKI_ENABLE_LOCAL_EMBEDDINGS=ON \
  -DWIKI_TEST_EMBEDDING_MODEL_PATH=/path/to/bge-small-en-v1.5-f16.gguf
cmake --build build --target local_embedding_provider_test
ctest --test-dir build -R local_embedding_provider_test --output-on-failure
```

### Testing against the real cloud API

This is gated at build time (`WIKI_ENABLE_CLOUD_EMBEDDINGS`) and skipped
at runtime (via Catch2's `SKIP`) when `OPENAI_API_KEY` isn't present in
the environment:

```sh
cmake -S . -B build -DWIKI_ENABLE_CLOUD_EMBEDDINGS=ON
cmake --build build --target cloud_embedding_provider_test
set -a && source .env && set +a   # or otherwise export a real OPENAI_API_KEY
ctest --test-dir build -R cloud_embedding_provider_test --output-on-failure
```

## Storage (`sqlite-vec`)

`EmbeddingIndexer` (`src/index/EmbeddingIndexer.{h,cpp}`) owns the
`sqlite-vec`-backed `document_embeddings` virtual table. It exposes
`ensureTable(dimensions, modelIdentifier)`, `upsertOne`/`removeOne`, and
`nearest()` (KNN via `vec0`'s own `MATCH`/`ORDER BY distance`).
`sqlite-vec` itself is vendored via `FetchContent` at a pinned commit,
compiled directly into `wikicore`, and registered once per process via
`sqlite3_auto_extension()` in `Database.cpp`. That file declares
`sqlite3_vec_init`'s prototype by hand rather than including
`sqlite-vec.h` directly, because that header pulls in `sqlite3ext.h`'s
macro-based `sqlite3_api` diversion table — meant only for a loadable
extension's own file — which would silently break every ordinary
`sqlite3_*` call elsewhere in the same translation unit.

`IndexUpdater::upsertOne`/`removeOne` call into `EmbeddingIndexer`
automatically whenever an `EmbeddingProvider*` is passed to their
constructor (it's optional, and defaults to `nullptr`). `main.cpp`
constructs the configured provider once at startup and passes it to
every `IndexUpdater` instance, including `VaultWatcher`'s own — so a
document changed by an external editor gets embedded too, not just one
saved through the web UI.

**A vector table's width is fixed at `CREATE` time.**
`CREATE VIRTUAL TABLE ... USING vec0(embedding float[N])` takes `N` as a
DDL literal, not a bind parameter, so one table can't hold both a
384-dimension and a 1536-dimension provider's vectors at once.
`EmbeddingIndexer::ensureTable()` tracks the table's current width (and
the provider's `modelIdentifier()`) in `index_meta`, and drops and
recreates the table empty on a mismatch. That's an accepted cost, since
the whole search index is already documented, project-wide, as
disposable and rebuildable from the vault.

### Chunking

A document is split into overlapping passage windows, rather than
embedded as one mean-pooled vector of its whole title and body. Two
reasons: a long note would otherwise drown a relevant paragraph in the
average, and a model with a small context window (512 tokens for
`bge-small-en-v1.5`) would reject the whole document outright once the
concatenated text tokenizes past that limit.

`embeddings::chunkForEmbedding` produces windows of roughly 384 estimated
tokens with 64-token overlap (capped at 32 chunks), each chunk prefixed
with the document title. `EmbeddingIndexer` stores one `vec0` row per
chunk, all sharing the same `document_rowid`, and `nearest()` collapses
chunks back down to unique documents using the best (minimum) chunk
distance, before RRF ever sees them. A short note still produces a
single `title + "\n\n" + body` chunk.

Token counts here are estimated (2 UTF-8 bytes per token), not computed
with the model's real tokenizer — this keeps `EmbeddingProvider`
tokenizer-agnostic. An oversized chunk still fails as a catchable
`embed()` error (see "Oversized documents" below) rather than crashing.

Embedding on save runs on a background worker: the HTTP Save response
returns as soon as the markdown and FTS5 index are committed, not after
embedding finishes. The embed worker coalesces by document — a second
Save of the same file replaces any job still waiting, and an identical
body that's already queued or in-flight is dropped rather than embedded
twice. `--reindex` / `POST /api/admin/reindex` still wait for embeddings
to finish (`IndexUpdater::flushEmbeddings()`), since a caller who
explicitly asked for a reindex wants to know when it's actually done.

### Oversized documents

`LocalEmbeddingProvider::embed()` checks the tokenized length against
`maxTokens_` (the model's own `n_ctx`, captured at construction) and
throws a plain `std::runtime_error` before calling `llama_decode()` if
the input is too long. This matters because llama.cpp's own internal
assert on an over-length input calls `abort()` directly, with no C++
exception a normal `catch` could intercept. A text longer than the
model's trained context window also produces a semantically meaningless
embedding even when it doesn't crash — so rejecting it outright is the
*correct* behavior here, not merely the safe one.

Chunking (above) means a long note should normally succeed as several
in-window passages, so this check rarely fires in practice. It stays
load-bearing for any caller that still calls `embed()` directly (an
admin retry, a future caller), and as a backstop in case the
estimate-based splitter ever produces a chunk the real tokenizer still
finds too long.

### Concurrency

`provider_->embed()` always runs with no lock held — a slow embed call
(a network round trip, or model inference) must never block other
threads' document saves. `LocalEmbeddingProvider` serializes its own
`embed()`/`embedQuery()` calls internally (`embedMutex_`), since
`llama_context`'s mutable KV-cache state isn't reentrant across threads
sharing one provider instance. `CloudEmbeddingProvider` needs no such
lock, since it constructs a fresh HTTP client per call.

Every actual database write this feature makes — document/FTS
transactions, and `EmbeddingIndexer`'s own `ensureTable()`/upsert
transactions alike — shares the single `IndexUpdater::mutex_`. Two
independent mutexes guarding the same underlying `sqlite3` connection
would not be safe, regardless of which part of the write path each one
is meant to protect.

## Incremental re-embedding

`document_embedding_state` (`src/index/schema.h`'s `kMigration5`) tracks,
per document, the `content_hash` (`std::hash` of `title + "\n\n" + body`)
as of its last *successful* embed, plus `last_error` (which is `NULL`
exactly when that attempt succeeded). `IndexUpdater::upsertOne` computes
the current hash and only queues a real `embed()` call when
`EmbeddingIndexer::needsEmbedding()` reports a mismatch — this is what
keeps a routine restart with no content changes fast.

A few rules worth knowing:

- **A failed embed attempt never touches `content_hash`, only
  `last_error`.** An old, still-accurate vector for unchanged content
  isn't invalidated by an unrelated later failure, and a document whose
  embed attempt failed stays permanently mismatched against its
  last-successful hash — so the next rescan or save retries it
  automatically, with no separate pending-retry queue needed.
- **A model swap forces a full re-embed, even at unchanged
  dimensionality.** Two different models can both produce 384-dimension
  vectors while encoding entirely incompatible vector spaces, so matching
  dimensions alone can't prove compatibility.
  `EmbeddingProvider::modelIdentifier()` (`"local:<path>:<mtime>:<size>"`
  or `"cloud:{api_base}:{model}"`) is compared alongside `dimensions()` in
  `ensureTable()`; a mismatch on either one drops and recreates
  `document_embeddings` and clears every row of
  `document_embedding_state`.
- **Documents below `embeddings.min_content_words` (default 6) skip
  embedding entirely.** A document with almost no prose doesn't carry a
  strong enough signal to land reliably on one side of any distance
  cutoff, for any query. The skip is recorded as success-with-no-vector
  (so it never shows up in "needing attention"), and clears any stale
  vector left over from a previously longer version of the same document.
  A later edit that adds enough content re-triggers a real embed through
  the normal content-hash mismatch.

## Hybrid ranking

`FtsSearch::search()` (in text-search mode only) blends BM25 and
cosine-distance ranking via Reciprocal Rank Fusion. `bm25CandidateRowIds()`
and `EmbeddingIndexer::nearest()` each independently rank up to 200
candidates, and RRF merges them by rank position
(`score += 1/(60 + rank)` per list, the standard `k=60` constant) — there's
no need to normalize BM25 scores and cosine distances onto a shared
scale, since only rank position matters. `limit`/`offset` apply to the
merged ranking. A rowid found only via the semantic side falls back to
the document's stored excerpt instead of an FTS5 `snippet()`, which has
no defined result outside a `MATCH` context.

Hybrid ranking falls back to plain FTS5-only ranking whenever it can't
proceed: no provider configured, no `document_embeddings` table yet, or
`embed()` itself failing. This is the same best-effort,
additive-never-load-bearing posture as the write path.

### Filters apply to both candidate sources

Tag/type/folder filters (`appendCommonFilterSql()`) apply identically to
the BM25 and semantic candidate sources — a semantic-only candidate that
fails an active filter is excluded the same way a BM25 candidate would
be.

One known limitation: filtering happens *after* the merged RRF list has
already been sliced by `limit`/`offset`, so a filtered-out semantic
candidate isn't backfilled from further down the ranked list. That means
an active filter can return fewer than `limit` results even when more
matches exist deeper in the full ranking. This isn't a correctness
problem — no wrong document is ever shown — just an incompleteness that
isn't worth fixing by joining tag/type into the semantic query itself,
for a vault this size.

### Relevance tuning

On a small vault, "the N closest neighbors" is effectively the whole
vault, and RRF has no way to reject an already-admitted candidate — only
to rank it. Three knobs bound what reaches RRF in the first place:

- **`embeddings.query_prefix`** (local only, empty by default). Some
  retrieval models (e.g. `bge-small-en-v1.5`) are trained with an
  instruction prefix on the query side only — that model's own card
  recommends `"Represent this sentence for searching relevant passages: "`.
  `EmbeddingProvider::embedQuery()` applies it, and
  `FtsSearch::tryHybridSearch()` always calls `embedQuery()`, never
  `embed()`, for the query side. `CloudEmbeddingProvider` needs no
  prefix, since OpenAI-compatible embeddings are symmetric.
- **`embeddings.max_distance`** (default `0.5`). A cosine-distance cutoff
  applied to `nearest()`'s results before they reach RRF.
- **`embeddings.semantic_top_k`** (default `5`). A hard cap on how many
  distance-cutoff-passing neighbors are admitted, taken in the
  nearest-first order `nearest()` already returns. A short, sharp query
  and a multi-concept query produce embeddings with very different
  "blurriness," so a single distance threshold alone can't bound both —
  capping by count does.

### Query embedding caching

Computing a query's embedding is the dominant cost of a hybrid search —
on this project's reference ARM target, `embedQuery()` alone takes
roughly 1.3–2.6 seconds, dwarfing the FTS5 `MATCH` query and the RRF
merge. `FtsSearch` caches `embedQuery()` results keyed on the raw query
text (`queryEmbeddingCache_`), since model identity and `query_prefix`
are both fixed for the lifetime of one `FtsSearch` instance
(`config.toml` is only read at startup).

The cache has a hard 256-entry cap, flushed wholesale on overflow — not a
real LRU. `/api/search` doesn't require authentication for public
content, so an unbounded map keyed on caller-supplied text would be a
memory-growth vector, and personal-wiki query volume doesn't justify real
LRU bookkeeping. The cache lookup and the `embedQuery()` call use
separate lock acquisitions, so a cache miss computing one query's
embedding doesn't block concurrent lookups for other query text.

## Admin visibility and control

There are two runtime-mutable settings, both SQLite-backed (no
`config.toml`/restart involved), plus an API surfacing the self-healing
state described above:

- **`EmbeddingsRuntimeConfig`** (`src/index/EmbeddingsRuntimeConfig.{h,cpp}`,
  `embeddings_runtime_config`) holds a `vectorSearchEnabled` bool, read by
  `FtsSearch::search()` on every call — not just at construction — so an
  admin can disable hybrid ranking live, without restarting
  `wiki-server`.
- **`GET/POST /api/admin/embeddings-status`**
  (`src/controllers/AdminRoutes.cpp`, admin+CSRF, registered only on a
  `WIKI_ENABLE_SQLITE_VEC` build). `GET` returns whether a real provider
  is configured, its dimensions/model identity, the runtime toggle state,
  and `EmbeddingIndexer::listNeedingAttention()`. `PUT` flips the toggle.
  `POST .../reembed` re-derives one document's index row via
  `IndexBuilder::reindexOneFile` (not a full `--reindex`); `POST
  .../reembed-all` does that for every currently-listed document and
  reports how many still fail. Both are logged to `mcp_audit_log`
  (`"admin:reembed"`/`"admin:reembed-all"`), visible via
  `GET /api/admin/mcp-audit-log`. `reembed-all` writes one summary entry
  per call, not one per document.
- **The Account page UI** (`static/js/pages/account.js`, "Semantic
  search (embeddings)" section) shows a checkbox bound to the runtime
  toggle, and a "Documents needing attention" list with per-document and
  bulk Retry buttons. The whole section simply doesn't render if
  `GET /api/admin/embeddings-status` returns `404` — i.e. on a build
  without embeddings compiled in.

`EmbeddingIndexer::listNeedingAttention()` returns every document with no
state row at all, or whose last attempt failed. It deliberately excludes
documents that are merely due for a routine hash-mismatch re-embed on
their next save — this list means "something needs a human to look at
it," not "this is due for normal work."

## Startup rescan

`IndexBuilder::fullRescan()` (unconditional at every `wiki-server`
startup — see `architecture.md`'s storage model) runs on a background
thread rather than blocking the HTTP listener. `main()` joins that thread
after `drogon::app().run()` returns, so a still-running rescan finishes
cleanly on shutdown instead of racing process teardown.

The background rescan shares the same `IndexUpdater`/connection as
request handlers, serialized by the same mutex — so a document saved
over HTTP while the rescan is still walking the vault gets indexed
immediately by that save. Search results fill in incrementally rather
than the whole site being unreachable until the rescan finishes.
`--reindex` / `POST /api/admin/reindex` stay fully synchronous, since a
caller who explicitly asks for a reindex wants to know when it's actually
done.

`RescanProgress` (`src/index/RescanProgress.h`, all-atomic, no mutex
needed) tracks `inProgress`/`documentsIndexed`, cleared via an RAII guard
so a rescan that throws partway through doesn't leave the tracker stuck
claiming "still running." `GET /api/admin/reindex-status` (admin-only, no
CSRF needed since it's a pure read of in-memory atomics) exposes
`{"inProgress":bool,"documentsIndexed":N}`; the account page polls it
every 1.5s and shows a self-hiding status line while a rescan is in
progress. This endpoint is registered on every build regardless of
whether embeddings are compiled in, since a full-vault rescan is relevant
either way.

**The real operational cost, with local embeddings enabled.** Because
every reindexed document also gets a real embedding computed
synchronously as part of the startup rescan, restart time now scales
with vault size under `provider = "local"` — roughly 1–1.5 seconds per
document on a constrained ARM SBC with no GPU. A vault of a few dozen
documents adds tens of seconds to every restart, during which `nginx`
(or whatever reverse proxy sits in front) serves `502` to anyone hitting
the site. Worth accounting for before enabling local embeddings on a
large vault, or before restarting at a moment someone might actually be
using the site. Memory footprint is dominated by the model itself
(roughly 100 MiB resident for `bge-small-en-v1.5`).

## `wiki-mcp` write tools

`wiki-mcp` constructs an `IndexUpdater` for its
`create_document`/`update_document`/`attach_file` tools, but does *not*
construct a real `EmbeddingProvider` at process startup. The reason:
`wiki-mcp` is spawned fresh per MCP session, and the overwhelmingly
common session never calls a write tool at all, so building a heavy
provider up front would be wasted work almost every time.

`mcp/McpServer.cpp`'s `LazyEmbeddingProvider` constructs the real
provider on the first actual write call (`IndexUpdater::setProvider()`);
every write after that first one reuses it. A failed lazy-init (a bad
model path, a missing API key) is logged to stderr once, and the write
still proceeds FTS5-only — the same best-effort posture `wiki-server`
itself uses for its embedding step.

`wiki-mcp`'s `search_documents` tool (and `FtsSearch` generally, when
constructed without a provider) stays FTS5-only on purpose. There's no
equivalent "first write" moment to lazily hook a provider into for a
read-only tool, and wiring one in just for reads would reopen the same
per-session startup cost this lazy design exists to avoid for writes.
