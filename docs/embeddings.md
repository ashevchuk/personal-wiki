# Semantic search (embeddings)

Semantic search blends two ranking signals: FTS5's lexical ranking, and
similarity ranking over vector embeddings. It's **additive to FTS5,
never a replacement** — with `[embeddings].provider = "none"` (the
default), FTS5 is the only search path, on every build.

There are two ways to generate those embeddings, because mandating a
single source would be wrong for a project whose documents are
fail-safe-private by default:

- **Cloud** (any OpenAI-compatible API) is the cheap, fast option, but
  every indexed document's content leaves the machine and goes to
  whatever host you point it at. That's fine as an explicit opt-in:
  against a loopback server (Ollama, LM Studio) nothing actually leaves
  the box; against `api.openai.com` it does.
- **Local** (a small quantized model run in-process) keeps every
  document on the machine, at the cost of a heavier binary and a real
  cross-compilation story for the ARM/SBC deployment path (see
  `sbc-deployment.md`).

Both are build-time-optional and independent of each other. A running
binary is then configured at runtime to use zero, one, or in principle
either.

## Build-time options

```
-DWIKI_ENABLE_LOCAL_EMBEDDINGS=ON   # links llama.cpp — a heavier build
-DWIKI_ENABLE_CLOUD_EMBEDDINGS=ON   # just an HTTP client — no new heavy dependency
```

Both default to **OFF**, so an SBC build stays as light as it is today
unless you opt in.

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
refuses to start, with an error naming the missing build flag. It never
silently falls back to `"none"` — that would leave FTS5 working fine
with no visible sign that semantic search never actually turned on.

**One caveat for cross-compiled ARM builds:** local embeddings
(`--local-embeddings`) via the zig toolchain is unverified — it builds,
but hasn't been exercised as thoroughly as the cloud path on that
target. `build cross --local-embeddings` prints a warning saying so.

## Runtime config (`config.toml`)

```toml
[embeddings]
provider = "none"        # "none" | "local" | "cloud"
# model_path = "..."     # local only — path to a GGUF model file
# api_key_env = "..."    # cloud only — NAME of an env var holding the API key,
                          # never the raw key itself (see config.example.toml);
                          # optional for a local server that doesn't authenticate
# api_base = "..."       # cloud only — OpenAI-compatible root including /v1
                          # (default https://api.openai.com/v1)
# model = "..."          # cloud only — JSON "model" field (default
                          # text-embedding-3-small)
# dimensions = 1536      # cloud only — vector width; MUST match the model
                          # (default 1536)
# query_prefix = "..."   # local only — prepended to search queries only, not
                          # documents (see "Relevance tuning" below)
# max_distance = 0.5     # cosine-distance cutoff for a semantic match (default 0.5)
# min_content_words = 6  # skip embedding entirely below this many words (default 6)
# semantic_top_k = 5     # hard cap on semantic candidates, on top of max_distance (default 5)
```

### OpenAI-compatible cloud endpoints

`provider = "cloud"` isn't locked to OpenAI itself — any server that
speaks the same request/response shape works (Ollama's
OpenAI-compatibility layer, LM Studio, Together, Fireworks, vLLM, a
local proxy). The binary must be built with
`-DWIKI_ENABLE_CLOUD_EMBEDDINGS=ON`.

**Defaults** (leave every knob unset and you get OpenAI itself):

| Knob | Default | Meaning |
|---|---|---|
| `api_base` | `https://api.openai.com/v1` | Absolute `http(s)` URL. The `/v1` prefix needs to be **in** this value for any server that uses one. |
| `model` | `text-embedding-3-small` | Sent as-is. |
| `dimensions` | `1536` | Vector width — must match the model. A mismatch is a clear error, not silent truncation. |
| `api_key_env` | unset | The **name** of an environment variable holding the API key, never the key itself. Unset means no `Authorization` header is sent — fine for local servers that don't authenticate. |

Switching the endpoint or model forces a full re-embed, even if the
vector width happens to stay the same — two same-width models aren't
necessarily the same vector space.

**What this is not.** Azure OpenAI, Gemini's native embeddings endpoint,
Voyage, and anything else that isn't this exact request shape. Pointing
`api_base` at those won't work.

**Where the key lives.** systemd's `EnvironmentFile=-/etc/opt/wiki/wiki.env`
(see `systemd/wiki.env.example`) is the documented place for
`WIKI_EMBEDDINGS_API_KEY=...`, or whatever name `api_key_env` points at.

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

## Relevance tuning

On a small vault, the default settings sometimes let too many loosely
related documents show up in semantic results. Three knobs bound that:

- **`query_prefix`** (local only) — some models expect a specific
  instruction prefix on search queries; leave empty unless your
  chosen model's documentation says otherwise.
- **`max_distance`** (default `0.5`) — how close a match needs to be to
  count at all. Lower is stricter.
- **`semantic_top_k`** (default `5`) — the most semantic matches that
  can be admitted per search, regardless of how many pass the distance
  cutoff.

A document shorter than `min_content_words` (default 6 words) is
skipped entirely for semantic indexing — too little text to produce a
vector that reliably means anything.

## Admin visibility and control

Two things are controlled live from the Account page, no restart
needed:

- **A toggle** to turn hybrid (semantic) ranking on or off without
  touching `config.toml`.
- **A "Documents needing attention" list** — anything that failed to
  embed, with per-document and bulk Retry buttons. A document here
  means something went wrong (a bad model path, an API error) — not
  that embedding is merely pending as part of normal operation.

Both are logged to the same admin audit log as everything else
(`GET /api/admin/mcp-audit-log`), and the whole section on the Account
page simply doesn't appear on a build without embeddings compiled in.

## The real operational cost of enabling local embeddings

Because every reindexed document also gets a real embedding computed as
part of the startup rescan, **restart time scales with vault size**
once `provider = "local"` is set — roughly 1–1.5 seconds per document
on a constrained ARM SBC with no GPU. A vault of a few dozen documents
can add tens of seconds to every restart, during which your reverse
proxy (if any) serves `502` to anyone hitting the site. Worth
accounting for before enabling local embeddings on a large vault, or
before restarting at a moment someone might actually be using the
site. Memory footprint is dominated by the model itself (roughly
100 MiB resident for a small model like `bge-small-en-v1.5`).

A routine restart with no actual content changes stays fast — documents
are only re-embedded when their content has actually changed since the
last successful embed, or when you switch models (which forces a full
re-embed of everything).
