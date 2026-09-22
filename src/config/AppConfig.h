#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace wikicore::config {

struct AppConfig {
  // [server]
  std::string listenAddr = "127.0.0.1";
  uint16_t port = 8080;
  size_t threads = 2;
  // Reverse-proxying under a subpath (e.g. "/wiki") needs NO server-side
  // configuration for the common case — there used to be a base_path
  // setting here for exactly that, removed once the frontend became a
  // fully client-rendered SPA shell + JSON API (see docs/architecture.md):
  // shell.html's own inline bootstrap script infers the mount prefix from
  // location.pathname at load time for every KNOWN route (/d/..., /search,
  // ...), correct automatically with nothing server-side needing to know
  // or care.
  //
  // Brought back here, OPTIONAL, empty by default, after a real deployment
  // hit the one case that client-side pattern-matching cannot ever close:
  // a request whose path matches NO known route (a typo, a stale
  // [[wiki-link]], someone's old bookmark) served by main.cpp's
  // setDefaultHandler, on a browser with nothing yet cached in
  // localStorage — no page load has happened yet to record a known-good
  // prefix, so there is no signal left ANYWHERE client-side to recover
  // it from; the earlier "cache the last known-good prefix" fallback
  // degrades gracefully but stays visibly broken on that first hit.
  // Setting this closes that gap completely: PageRoutes.cpp bakes it into
  // EVERY served shell.html as an authoritative
  // `window.__WIKI_KNOWN_BASE_PATH__`, which the bootstrap script uses
  // instead of guessing, matched route or not. Leave unset for a
  // deployment on its own (sub)domain — pattern-matching already covers
  // that case perfectly, nothing to gain by setting it.
  std::string basePath;

  // Optional, empty by default (meaning "no server-side opinion — the
  // client picks its own hardcoded fallback, currently green"). Set to
  // "classic", "dark", or "green" to make a FRESH browser (no
  // wiki.theme in localStorage yet) land on that theme instead. Same
  // injection mechanism as basePath just above: PageRoutes.cpp bakes
  // this into every served shell.html as
  // `window.__WIKI_DEFAULT_THEME__`, which the bootstrap script's own
  // fallback chain checks before its own hardcoded "green" — a reader
  // who's already picked a theme via the sidebar picker always keeps
  // that choice regardless of this setting; it only affects the very
  // first, cold visit. Deliberately NOT validated here (same as
  // mcpScope just below — a string passed through as-is, no allowlist
  // at this layer): the bootstrap script already checks any value
  // against its own THEMES array before using it, so a typo'd or
  // stale theme name here just falls through to "green" client-side,
  // exactly as if this had been left unset. No reason to duplicate
  // that allowlist on the C++ side too.
  std::string defaultTheme;

  // [vault]
  std::string vaultPath = "./vault_data";

  // [index]
  std::string dbPath = "./vault_data/.index.db";

  // [mcp]
  // "admin" — sees public + private (default; local stdio spawn by owner).
  // "public" — sees only public. Reserved for a future remote transport.
  std::string mcpScope = "admin";
  // Phase 2: create_document/update_document tools. Defaults OFF — MCP
  // clients (an LLM) writing to the vault unsupervised is a materially
  // different risk than read-only search/browse, worth an explicit,
  // conscious opt-in rather than showing up silently the first time
  // this binary gets rebuilt. Every write through these tools (success
  // OR failure) is recorded in the mcp_audit_log table regardless —
  // see McpServer.cpp — so turning this on is "let the LLM write,
  // reviewably", not "let it write invisibly".
  bool mcpWriteAccess = false;

  // [attachments] — empty means "use AttachmentService's own built-in
  // defaults" (see main.cpp and AttachmentService::defaultMimeTypes() /
  // defaultInlineSafeExtensions()); config.toml intentionally isn't
  // required to duplicate that whole list just to run. If
  // [attachments.mime_types] / inline_safe_extensions IS present in
  // config.toml, it REPLACES the built-in list entirely (not merged) —
  // see config.example.toml's comment on why, and copy the full default
  // list there if you just want to add one entry.
  std::unordered_map<std::string, std::string> attachmentMimeTypes;
  std::unordered_set<std::string> attachmentInlineSafeExtensions;

  // [embeddings] — see docs/embeddings.md. "none" (default) means FTS5 is
  // the only search path; always valid regardless of how this binary was
  // built. "local"/"cloud" additionally require the matching
  // WIKI_ENABLE_LOCAL_EMBEDDINGS/WIKI_ENABLE_CLOUD_EMBEDDINGS build flag —
  // EmbeddingProviderFactory throws a clear startup error naming the
  // missing flag rather than silently falling back to "none" when it
  // doesn't (see docs/embeddings.md's "Fail loudly, not silently").
  std::string embeddingsProvider = "none";
  // Local only — path to a GGUF model file. Ignored for "none"/"cloud".
  std::string embeddingsModelPath;
  // Cloud only — the NAME of an environment variable holding the API key,
  // never the raw key itself: config.toml is plain text and sometimes
  // gets pasted into issues/screenshots, an env var doesn't accidentally
  // travel with it. Empty is allowed (some OpenAI-compatible local
  // servers don't authenticate). Ignored for "none"/"local".
  std::string embeddingsApiKeyEnv;
  // Cloud only — OpenAI-compatible API root, including the `/v1` prefix
  // `{api_base}/embeddings` is posted to. Empty means OpenAI's own
  // `https://api.openai.com/v1`. Any server that speaks that POST + JSON
  // shape works (Ollama, LM Studio, Together, a self-hosted vLLM, …).
  // Ignored for "none"/"local".
  std::string embeddingsApiBase;
  // Cloud only — JSON `"model"` field. Empty means OpenAI's
  // `text-embedding-3-small`. Ignored for "none"/"local" (`model_path`
  // is the local GGUF file, a different thing).
  std::string embeddingsCloudModel;
  // Cloud only — vector width this endpoint/model returns. sqlite-vec
  // needs this at CREATE time, before any HTTP call, so it cannot be
  // inferred from the first response. 0 means 1536 (the default model's
  // width). A mismatch with what the API actually returns fails embed()
  // rather than silently truncating. Ignored for "none"/"local".
  std::size_t embeddingsCloudDimensions = 0;
  // Local only — prepended to a search QUERY (never to a document/passage)
  // before embedding it, via EmbeddingProvider::embedQuery(). Empty
  // (default) means no prefix. Many small local retrieval models,
  // including bge-family ones, are trained with an instruction prefix on
  // the query side only — bge-small-en-v1.5's own model card recommends
  // "Represent this sentence for searching relevant passages: ". Found
  // live to matter a lot for result relevance — see docs/embeddings.md's
  // "hybrid search relevance" writeup for real measured numbers. Ignored
  // for "none"/"cloud" (OpenAI-compatible embeddings are typically
  // symmetric — no query-side convention to apply).
  std::string embeddingsQueryPrefix;
  // Local/cloud — the maximum cosine DISTANCE (1 - cosine similarity; 0 =
  // identical, 2 = opposite) a document's embedding may have from the
  // query's before FtsSearch::tryHybridSearch() lets it into the semantic
  // candidate list RRF blends with BM25 results at all. Without this,
  // EmbeddingIndexer::nearest() returns the closest N neighbors with NO
  // relevance floor — on a small vault (the common case for this
  // project), that's effectively the WHOLE vault, ranked by a distance
  // that's often just noise for a genuinely unrelated document, and RRF
  // then gives every one of them a nonzero score. Found live on real
  // production content: a plain one-word query returned most of an
  // unrelated vault (recipes, a welcome page, empty demo docs) alongside
  // the few actually-relevant results. Default chosen empirically against
  // real bge-small-en-v1.5 measurements — see docs/embeddings.md — not
  // guessed; tune per-model if a different one is configured.
  double embeddingsMaxDistance = 0.5;
  // Local/cloud — a document whose title+body combined has FEWER
  // whitespace-separated words than this never gets a real embed() call
  // at all (IndexUpdater::upsertOne()) — found live: a near-empty
  // document (a title plus just an image link, no real prose) produces a
  // vector whose semantic signal is too weak to reliably land far from
  // queries it has nothing to do with, and no distance threshold alone
  // fixes a document whose OWN vector doesn't carry a strong enough
  // signal. Default (6) chosen to exclude a real measured stub page
  // (4 words) while including short-but-real content (an 8-word test
  // sentence stays eligible) — see docs/embeddings.md.
  int embeddingsMinContentWords = 6;
  // Local/cloud — a hard cap on how many of the nearest neighbors are
  // even allowed into the semantic candidate list RRF blends with BM25,
  // on TOP of the distance cutoff above, not instead of it. Found live
  // re-checking the distance-cutoff fix against more real production
  // queries: a multi-word query's embedding can sit at a uniformly
  // "blurry" distance from MANY unrelated documents at once on a small
  // vault, each individually still under embeddingsMaxDistance — RRF has
  // no way to reject a candidate once it's in the ranked list, only rank
  // it (see FtsSearch.h's own comment), so a threshold alone can't bound
  // how many mediocre matches flood in for that kind of query. Default
  // (5) chosen against the same real measurements as the distance cutoff
  // — see docs/embeddings.md.
  int embeddingsSemanticTopK = 5;

  // [llm] — outbound chat for the editor's Draft panel. "none" (default)
  // means the Draft button is not shown and /api/agent/* 404s. "cloud" is
  // an OpenAI-compatible POST {api_base}/chat/completions (OpenAI itself,
  // Anthropic's compatibility layer at api.anthropic.com/v1, a local
  // proxy). The wiki is the HTTP client; the model is never given MCP.
  // Key is the NAME of an env var, same discipline as embeddings.api_key_env.
  std::string llmProvider = "none";
  std::string llmApiKeyEnv;
  std::string llmApiBase;
  std::string llmModel;
  // Optional override of the compiled Draft agent system prompt. Empty
  // (unset, commented out, or whitespace-only) keeps the hardcoded
  // default in AgentRuntime.
  std::string llmSystemPrompt;

  // [log]
  std::string logLevel = "info";

  // Loads `path` if it exists (TOML, see config.example.toml); returns
  // defaults unchanged if the file is absent. Throws on a file that exists
  // but fails to parse — a broken config should never be silently ignored.
  static AppConfig load(const std::string& path);
};

}  // namespace wikicore::config
