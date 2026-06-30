# Memory embeddings — design spec

**Date:** 2026-06-30
**Status:** approved (brainstorm via Q&A; design confirmed "yes"). Ready for plan.

## Goal

Add **semantic** memory retrieval to hades as a separate **opt-in MOOS-app-style Module** (`embedding_memory`)
in the `Module=` roster. Absent → today's behavior (keyword-ranked archival memory + `pin_fact` core facts).
Present → the agent also recalls memories by *meaning* (embeddings/cosine), over the archival store **and** the
past-session corpus, using a **local embedder binary anywhere on the system** (primary) or an OpenAI-compatible
HTTP `/embeddings` endpoint (remote or local-HTTP). Keyword and semantic run **together (hybrid)**.

The author (Vaios) has stated he does not know embeddings well and cannot catch a modeling mistake — so this spec
is deliberately explicit about the load-bearing correctness rules (model/dimension consistency, normalization,
fail-soft), and ships a reference embedder so there is a known-good path.

## Background: how this works (so the design choices are legible)

- An **embedding model** maps text → a fixed-length vector of floats (e.g. 384). Similar *meaning* → vectors
  pointing in similar directions. Similarity = **cosine** (dot product of L2-normalized vectors), ~ −1..1.
- **THE load-bearing rule:** the *same* model (and therefore the same dimension) must embed both the stored
  records **and** the query. Vectors from different models/dims are **incomparable** — comparing them yields
  garbage. The cache is therefore **model-stamped**; a model/dim change forces a rebuild.
- **Two distinct operations** (only the first is the periodic batch):
  1. **Index** (docs): embed every stored memory + past-session-turn → cache the vectors. Many texts, slow,
     runs on launch + optionally daily, in the background. *Not* per turn.
  2. **Query** (live): each user message is new, so it must be embedded *that turn* (1 text) and cosine-compared
     against the cache. Cannot be precomputed.
- **Warm process:** the per-turn query-embed is cheap **only** if the embedder is already loaded. So the
  subprocess embedder is a **long-lived warm process** (model loads once at startup, then answers requests over a
  pipe), reused for both index and query. Spawn-per-call would reload the model every query (seconds) — rejected.

## Components

### 1. `EmbeddingProvider` — interface + two implementations
`include/hades/embedding/provider.h`:
```cpp
struct EmbedResult { std::vector<std::vector<float>> vectors; std::string model; int dim = 0; std::string error; };
class EmbeddingProvider {
public:
  virtual ~EmbeddingProvider() = default;
  // Embed a batch; returns one vector per input (same order). On failure, error is non-empty and the caller
  // MUST fail-soft (skip indexing those records / return empty semantic recall) — never throw into the bus.
  virtual EmbedResult embed(const std::vector<std::string>& texts) = 0;
  virtual std::string model() const = 0;   // the model id this provider produces (stamped into the cache)
};
```

- **`SubprocessEmbeddingProvider`** (primary — "any embedder binary, anywhere"): launches the configured
  command **once** (warm process), then per request writes ONE json line to its stdin and reads ONE json line
  from its stdout. **Persistent line protocol** (NOT one-shot, NOT JSON-RPC):
  - request:  `{"texts":["t1","t2",...]}\n`
  - response: `{"model":"<name>","dim":<int>,"embeddings":[[..],[..]]}\n`  (one vector per input, in order)
  - error:    `{"error":"<msg>"}\n`  (or the process dies / times out)
  - hades **validates** every response: `embeddings.size()==texts.size()`, every vector length `==dim`, `dim`
    consistent with the configured/cached dim. Any violation → fail-soft (that batch yields no vectors), logged once.
  - Lifecycle: started in `on_attach` (or lazily on first use), killed at module teardown. Crash/EOF/timeout →
    mark dead, attempt one restart on next use; persistent failure → provider returns `error` (keyword still works).
  - Uses the existing `run_subprocess` plumbing's child-spawn pattern but keeps the pipes open across calls
    (a small persistent-child helper; the one-shot `run_subprocess` is for tools).
  - **Concurrency (load-bearing):** the background index task (on an Executor worker) and the per-turn query (on
    the pump thread) both call `embed()` on the **one** warm child. Concurrent writes to the child's stdin would
    interleave and corrupt the protocol — so `embed()` **serializes with an internal mutex** (one request/response
    round-trip at a time). A long index batch therefore briefly delays a concurrent query-embed; acceptable
    (batches are bounded by `batch_size`, and the query fail-softs on timeout). This mutex is the single point that
    makes the shared warm child thread-safe.
- **`HttpEmbeddingProvider`**: OpenAI-compat `POST {endpoint}/embeddings` with `{"model":..,"input":[texts]}`,
  parses `data[i].embedding`; reuses the `HttpClient` seam (`cpr_http`) + `api_key_env` (Bearer). Endpoint at
  PPQ/OpenAI = remote; at `http://localhost:11434/v1` (ollama) / llama.cpp-server / TEI = local-HTTP. Same model
  consistency + validation + fail-soft as above.
- Tests inject a **fake** `EmbeddingProvider` (deterministic vectors) — no real model in unit tests, exactly as
  the LLM provider tests inject a fake `Provider`.

### 2. Vector cache — `.hades/embeddings/<source>.vec.jsonl`
- One json object per line: `{"id":"<stable>","src":"memory|session:<file>#<turn>","model":"<m>","dim":N,
  "text":"<original, possibly truncated for display>","vec":[f,...]}`.
- **`id` is stable** so incremental indexing skips already-embedded records:
  - archival memory: `memory#<line-index>` (the store is append-only; index is stable).
  - session turn: `session:<basename>#<turn-index>`.
- **Model-stamped:** every line carries `model`+`dim`. On load, if the configured provider's `model` ≠ the
  cache's stamped model (or dim differs) → **loud log + rebuild that cache from scratch** (the only safe action;
  never compare mismatched vectors). Reader is tolerant of blank/corrupt/partial-trailing lines (the
  `read_session_jsonl` discipline) — a corrupt line is skipped, not fatal.
- Loaded into memory as `{id → (vec, text, src)}` for cosine; **vectors are L2-normalized at write time** so the
  query path is a plain dot product. A degenerate zero-vector (norm 0) is dropped with a warning.
- Separate cache file per source family keeps memory vs session corpora independently rebuildable.

### 3. Indexer — lazy, incremental, background
- **Trigger:** the index always runs **incrementally** (compute the set of `id`s already cached, embed only the
  *new* records in `batch_size` batches, append to the cache). It runs **(a) once at launch** — so a fresh or
  updated store is ready immediately; a launch with no new records is a **no-op** — **and (b) every
  `reindex_interval_s`** thereafter (default **86400 = daily**) to pick up sessions that completed during a
  long-running process. Both runs are the same incremental task on an **Executor** worker (startup/bus not
  blocked; reuses the warm provider, no respawn). `reindex_interval_s = 0` ⇒ launch-only (no periodic timer).
  The first build (cold cache) is the slow one (logged with a count); the module answers semantic queries with
  whatever is cached and is empty (keyword-only) until that first index completes.
- **Reuses existing loaders (no new format):** the archival corpus is read with the existing **`load_memories`**
  (`.hades/memory.jsonl` → `MemoryRecord`); the session corpus is read with the existing **`read_session_jsonl`**
  (the GET /history reader) → the raw message array, transformed into per-turn pairs. The embedder consumes
  exactly what `save_memory` and session-resume already persist.
- **Corpus:**
  - archival `memory.jsonl` → 1 `MemoryRecord` per line, embed `record.text` = 1 vector.
  - past `sessions_dir/*.jsonl` (when `index_sessions=true`) → **per turn**: each `{role:user}` paired with the
    following `{role:assistant, content:string}` (intervening tool turns folded into that pair's text as
    `"U: <user>\nA: <assistant>"`); 1 vector per turn. **The live/current session file is excluded** (it is
    mid-write and already in the Arbiter's working context); identified by the active session path passed in.
- A standalone re-index entry point is **deferred** (lazy covers the need); noted for v2 if a manual `--reindex`
  is wanted.

### 4. `EmbeddingMemoryModule` (`type()=="embedding_memory"`) — the query path
- `on_start`: parse the `Embedding` block (provider kind + provider config; `cache_dir`, `memory_store`,
  `sessions_dir`, `index_sessions`, `top_n` default 5, `min_similarity` default 0.25, `reindex_interval_s`
  **default 86400** (daily; `0` = launch-only), `batch_size` default 32, `timeout_s` default 120). Bad/garbage
  values → defaults (never 0/empty silently).
- `on_attach`: construct the provider; kick off the background index (3); subscribe `USER_MESSAGE`.
- On `USER_MESSAGE`: embed **only the query** (1 provider call, fail-soft on error/timeout → empty), L2-normalize,
  cosine vs the in-memory cache, drop matches below `min_similarity` (the weak-match floor — cosine always
  returns *something*, the floor stops noise injection), take `top_n`, render `"- <text>\n"` bullets, post
  **`RETRIEVED_MEMORY_SEMANTIC`** (`""` when nothing clears the floor or the provider failed). The query-embed
  is **inline on the pump thread** in v1 — with the warm provider it is ~10–50ms, dwarfed by the LLM call;
  offloading it (so retrieval feeds the prompt without any pump pause) is a documented v2.

### 5. Arbiter — hybrid dedup injection
- Today the Arbiter injects `RETRIEVED_MEMORY` as an ephemeral `{role:system}` "Relevant memories:" block before
  the last user message. Change: also read the latest **`RETRIEVED_MEMORY_SEMANTIC`**, **merge** the two bullet
  lists, **dedup identical lines** (a record both methods surface appears once), and inject the single combined
  block. Empty/absent semantic key → byte-identical to today (backward-compatible; the existing memory tests
  stay green). Both producers run on the pump thread in subscription order before the Arbiter's `start_turn`
  reads them (the existing keyword-before-Arbiter wiring-order guarantee extends to the embedding module — it
  must be wired/subscribed before the Arbiter, like `memory`).

### 6. Manifest config (multi-line block — the parser fails LOUD on packed lines)
```
Module = memory             # keyword (kept — hybrid)
Module = embedding_memory   # the new semantic app
Embedding {
  provider           = subprocess
  command            = /path/to/your-embedder --model minilm
  # provider = http
  # endpoint   = http://localhost:11434/v1
  # model      = nomic-embed-text
  # api_key_env = HADES_EMBED_KEY
  cache_dir          = .hades/embeddings
  memory_store       = .hades/memory.jsonl
  sessions_dir       = .hades/sessions
  index_sessions     = true
  top_n              = 5
  min_similarity     = 0.25
  reindex_interval_s = 86400   # daily (default); 0 = launch-only
  batch_size         = 32
  timeout_s          = 120
}
```
`dev.hades` stays **keyword-by-default**; this block + the module line is the opt-in. Provider config is
whitespace-validated like other store paths where the existing wiring rules require it; the embedder `command`
is split like a tool argv (document the same no-quoted-spaces v1 limit, or pass args as separate keys).

### 7. Reference embedder (known-good path)
- Ship `tools/embed_reference.py`: ~20 lines, loads `sentence-transformers all-MiniLM-L6-v2` (384-dim, tiny,
  CPU-fast, `pip install sentence-transformers`) **once**, then loops reading stdin lines and writing the
  contract's response lines (the persistent warm protocol). This is the documented, tested local path.
- Document the HTTP alternative: ollama (`ollama pull nomic-embed-text`, `endpoint=http://localhost:11434/v1`)
  or llama.cpp-server / text-embeddings-inference — all OpenAI-compat `/embeddings`.

## Data flow

```
launch (Module roster has embedding_memory)
  on_attach: start warm embedder; Executor task -> incremental index(memory.jsonl + past sessions) -> cache
  [reindex_interval_s>0] Executor timer -> incremental reindex every interval

turn:
  USER_MESSAGE
    -> MemoryModule (keyword)        -> RETRIEVED_MEMORY
    -> EmbeddingMemoryModule         -> embed query (warm, ~10-50ms) -> cosine vs cache -> floor -> top_n
                                     -> RETRIEVED_MEMORY_SEMANTIC
    -> Arbiter.start_turn: inject merge(dedup(RETRIEVED_MEMORY, RETRIEVED_MEMORY_SEMANTIC)) as the
       "Relevant memories:" system block, then send LLM_REQUEST
```

## Error handling / safety (the author cannot catch a modeling mistake — fail closed, never crash)

- Embedder missing / non-zero exit / EOF / timeout / malformed json / count-or-dim mismatch → **log once,
  fail-soft**: index skips those records; a query returns empty `RETRIEVED_MEMORY_SEMANTIC`; the keyword module
  still answers. A turn is **never** crashed by the embedder.
- **Model/dim mismatch vs the cache** → loud log + rebuild that cache (never silently compare incomparable
  vectors). The query path asserts query-model==cache-model and fails-soft if it somehow differs.
- All external json (provider responses, cache lines) is **bounds/type-checked** before use (the capability/SSRF
  discipline — never trust external data; e.g. a non-number where a float is expected → skip the record).
- Warm child process: killed at teardown (teardown-order respected so the Executor and child are joined/reaped
  before the Blackboard/modules go away — the existing load-bearing teardown contract).
- The cache + corpus live under the gitignored `.hades/`; tool/session content is the same trust boundary as the
  Eventlog. No api key in the cache.

## Testing (TDD)

- **Provider:** inject a fake `EmbeddingProvider` (deterministic vectors); assert batch order, model/dim stamping,
  and that a fake error path is surfaced as `error` (not a throw). For `SubprocessEmbeddingProvider`, a tiny test
  echo-embedder script asserts the persistent line protocol (two requests over one process; restart-on-EOF).
  `HttpEmbeddingProvider` uses the injected `HttpClient` (no network), like the LLM provider tests.
- **Cache:** round-trip write→read; model-mismatch triggers a rebuild; tolerant of corrupt/partial lines;
  normalized vectors (norm≈1); zero-vector dropped.
- **Cosine/retrieval:** known vectors → correct ordering; `min_similarity` floor excludes weak matches; `top_n`
  caps; empty cache → empty result.
- **Indexer:** incremental skip (already-cached `id`s not re-embedded); session per-turn extraction (user+assistant
  pairing, tool turns folded, live session excluded); first-build count.
- **Module:** `USER_MESSAGE` → `RETRIEVED_MEMORY_SEMANTIC` posted; provider failure → empty (fail-soft), no crash.
- **Arbiter:** merge+dedup of the two keys into one injected block; semantic absent → identical to today (existing
  memory/injection tests stay green).
- **Live smoke (Vaios, with the reference embedder):** index a real store + a couple sessions, ask a paraphrased
  question, confirm a semantically-relevant memory is recalled that keyword alone would miss; confirm fail-soft
  when the embedder is stopped.

## Phasing (one spec; the plan sequences it — each phase ships working + testable)

- **P1 — semantic recall over archival memory:** `EmbeddingProvider` (iface + subprocess-warm + http + fake),
  vector cache (model-stamped, normalized, tolerant), cosine+floor+top_n, lazy incremental index of
  `memory.jsonl`, `EmbeddingMemoryModule`, Arbiter dedup-merge injection, manifest `Embedding` block + wiring,
  reference embedder. Hybrid with keyword. Live-validated.
- **P2 — session corpus + periodic reindex:** per-turn extraction over `sessions_dir/*.jsonl` (live excluded),
  `index_sessions`, `reindex_interval_s` Executor timer.

## Out of scope (noted)

- Offloading the per-turn query-embed off the pump thread (v1 inline + warm = ~10–50ms; offload = v2).
- A manual `--reindex` CLI (lazy covers it).
- Vector DB / ANN index (flat cosine is fine at hundreds–thousands of records; sqlite/faiss/binary format = later).
- Dedup/decay/importance of memories; LLM auto-summarization of sessions; chunking long single turns.
- Multiple embedding models / per-corpus models (one model, stamped).

## MOOS-IvP framing

`embedding_memory` is a **separate sensor app on the community's blackboard**: like a MOOS app you add to the
mission only when you want that capability, it subscribes to the same `USER_MESSAGE` the keyword memory app does
and publishes its own retrieval channel — the helm (Arbiter) fuses both. Omit the app and the community runs
exactly as before; add it (and point it at a local or remote embedder) and the agent gains semantic recall over
its own logged missions. The warm embedder process is the app's dedicated sensor driver, started with the app and
reaped with it.
