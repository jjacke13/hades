# Memory embeddings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax. **Implementers run on OPUS.** Read the named files fully before editing. **DISCIPLINE:** build + FULL `ctest` suite + `git commit` + verify `git log` shows your commit + write report BEFORE replying DONE.

**Goal:** Add opt-in semantic memory retrieval as a separate `embedding_memory` Module — embeddings over the archival store (P1) and the past-session corpus (P2), via a warm local embedder subprocess or an OpenAI-compat HTTP endpoint, running **hybrid** alongside the keyword `memory` module.

**Architecture:** An `EmbeddingProvider` (warm-subprocess or HTTP) turns text → vectors. A model-stamped on-disk vector cache holds embedded corpus records; an incremental indexer (Executor-backed, launch + daily) fills it. `EmbeddingMemoryModule` embeds each user query, cosine-ranks the cache above a floor, and posts `RETRIEVED_MEMORY_SEMANTIC`; the Arbiter merges+dedups that with the keyword `RETRIEVED_MEMORY` into one injected block. Everything fail-soft: any embedder failure degrades to keyword-only, never crashes a turn.

**Tech Stack:** C++20 · CMake + Ninja · Nix dev shell (`nix develop`) · cpr (`cpr_http` seam) · nlohmann/json · POSIX fork/exec (warm child) · GoogleTest · a Python reference embedder.

Spec: `docs/superpowers/specs/2026-06-30-memory-embeddings-design.md` (read first).

## Global Constraints

- **C++20**, g++ 15.2. **Every build/test command inside `nix develop`** (`nix develop --command cmake --build build`, `nix develop --command ctest --test-dir build`).
- **TDD:** RED → GREEN → COMMIT per task. Commit `<type>: <desc>`, **NO attribution footer / NO Co-Authored-By**.
- **Backward-compat:** the existing **204** tests stay green. The embedding module is **inert unless the manifest lists `Module = embedding_memory`** — the test `build_agent` overload never builds it, and the Arbiter merge is byte-identical to today when `RETRIEVED_MEMORY_SEMANTIC` is absent/empty.
- **THE load-bearing rule:** the *same* model+dim must embed corpus AND query — vectors from different models/dims are incomparable. The cache is **model-stamped**; a mismatch forces a rebuild, never a silent compare.
- **Fail-soft everywhere:** a missing/erroring/timing-out embedder, malformed json, or model/dim mismatch → log once, degrade to keyword, **never crash a turn**. All external json is type/bounds-checked before use.
- **Normalization:** every stored + query vector is L2-normalized so cosine == dot product; a zero-norm vector is dropped.
- **Concurrency:** the one warm child is shared by the background index task and the per-turn query → the subprocess provider **serializes `embed()` with a mutex**.
- **Manifest:** the `Embedding` block is **multi-line** (the parser fails LOUD on packed `k=v` lines). Provider config paths are whitespace-free where wiring requires it.
- **Test seams (mirror LLMModule):** `EmbeddingMemoryModule` takes an **injected `EmbeddingProvider`** ctor for tests (no real model); with **no Executor set** the index runs **inline** (deterministic tests), exactly as the LLM runs inline without an executor.
- **Defaults (single-sourced in `include/hades/embedding/defaults.h`):** `top_n=5`, `min_similarity=0.25f`, `batch_size=32`, `timeout_s=120.0`, `reindex_interval_s=86400.0` (daily; `0`=launch-only).

## File Structure

```
include/hades/embedding/defaults.h               T1 (new)  shared default constants
include/hades/embedding/provider.h               T1 (new)  EmbeddingProvider iface + EmbedResult
include/hades/embedding/vec_math.h               T1 (new)  l2_normalize / dot
src/embedding/vec_math.cpp                       T1 (new)
tests/test_embedding_vec_math.cpp                T1 (new)

include/hades/embedding/http_embedding_provider.h  T2 (new)
src/embedding/http_embedding_provider.cpp          T2 (new)
tests/test_http_embedding_provider.cpp             T2 (new)

include/hades/embedding/persistent_child.h       T3 (new)  warm fork/exec line-protocol child
src/embedding/persistent_child.cpp               T3 (new)
include/hades/embedding/subprocess_embedding_provider.h  T3 (new)
src/embedding/subprocess_embedding_provider.cpp          T3 (new)
tests/test_subprocess_embedding_provider.cpp     T3 (new)  uses a tiny echo-embedder script
tests/fixtures/echo_embedder.py                  T3 (new)

include/hades/embedding/vector_cache.h           T4 (new)  CachedVec/ScoredMemory + VectorCache
src/embedding/vector_cache.cpp                   T4 (new)
tests/test_vector_cache.cpp                      T4 (new)

include/hades/embedding/indexer.h                T5 (new)  incremental archival indexer
src/embedding/indexer.cpp                        T5 (new)
tests/test_indexer.cpp                           T5 (new)

include/hades/module/embedding_memory_module.h   T6 (new)
src/module/embedding_memory_module.cpp           T6 (new)  on_start/on_attach + provider factory
tests/test_embedding_memory_module.cpp           T6 (new)

src/arbiter/arbiter.cpp                          T7 (modify) merge RETRIEVED_MEMORY + _SEMANTIC
tests/test_arbiter.cpp                           T7 (extend)

app/agent_wiring.{h,cpp}                          T8 (modify) Agent.embedding + factory + wire
app/hades_main.cpp                                T8 (modify) (no change expected; verify)
manifests/dev.hades                               T8 (modify, commented opt-in example)
tools/embed_reference.py                          T8 (new)  known-good sentence-transformers embedder
docs (CLAUDE.md)                                  T8 (modify)
tests/test_embedding_wiring.cpp                   T8 (new)

include/hades/embedding/session_turns.h          T9 (new)  per-turn extraction (pure)
src/embedding/session_turns.cpp                  T9 (new)
tests/test_session_turns.cpp                     T9 (new)

src/embedding/indexer.cpp                        T10 (modify) session corpus + live-exclusion
include/hades/embedding/indexer.h                T10 (modify)
tests/test_indexer.cpp                           T10 (extend)

src/module/embedding_memory_module.cpp           T11 (modify) periodic reindex timer thread
include/hades/module/embedding_memory_module.h   T11 (modify)
tests/test_embedding_memory_module.cpp           T11 (extend)
```

CMake: every new `src/embedding/*.cpp` → `target_sources(hades_core PRIVATE ...)`; every `tests/test_*.cpp` → `target_sources(hades_tests PRIVATE ...)` (mirror the lines at `CMakeLists.txt:18-86`). T3's test needs the fixture path as a compile def (see T3).

---

# PHASE 1 — semantic recall over the archival store

## Task 1: EmbeddingProvider interface + vector math

**Files:** Create `include/hades/embedding/defaults.h`, `include/hades/embedding/provider.h`, `include/hades/embedding/vec_math.h`, `src/embedding/vec_math.cpp`, `tests/test_embedding_vec_math.cpp`. Modify `CMakeLists.txt`. **Read first:** `CMakeLists.txt:18-37`, `include/hades/memory/record.h`.

**Interfaces — Produces:** `EmbeddingProvider`/`EmbedResult`; `bool l2_normalize(std::vector<float>&)`; `float dot(const std::vector<float>&, const std::vector<float>&)`; the default constants.

- [ ] **Step 1: Failing tests** — `tests/test_embedding_vec_math.cpp`:

```cpp
#include <gtest/gtest.h>
#include <cmath>
#include <vector>
#include "hades/embedding/vec_math.h"
using namespace hades;

TEST(VecMath, NormalizeMakesUnitLength) {
  std::vector<float> v{3.0f, 4.0f};            // norm 5
  ASSERT_TRUE(l2_normalize(v));
  EXPECT_NEAR(v[0], 0.6f, 1e-5);
  EXPECT_NEAR(v[1], 0.8f, 1e-5);
  EXPECT_NEAR(std::sqrt(v[0]*v[0] + v[1]*v[1]), 1.0f, 1e-5);
}
TEST(VecMath, NormalizeZeroVectorReturnsFalse) {
  std::vector<float> z{0.0f, 0.0f, 0.0f};
  EXPECT_FALSE(l2_normalize(z));               // degenerate -> caller drops
}
TEST(VecMath, DotOfNormalizedIsCosine) {
  std::vector<float> a{1.0f, 0.0f}, b{1.0f, 0.0f}, c{0.0f, 1.0f};
  l2_normalize(a); l2_normalize(b); l2_normalize(c);
  EXPECT_NEAR(dot(a, b), 1.0f, 1e-5);          // identical -> 1
  EXPECT_NEAR(dot(a, c), 0.0f, 1e-5);          // orthogonal -> 0
}
TEST(VecMath, DotSizeMismatchIsZero) {
  std::vector<float> a{1.0f, 0.0f}, b{1.0f};
  EXPECT_FLOAT_EQ(dot(a, b), 0.0f);            // defensive: incomparable -> 0
}
```

- [ ] **Step 2: Register + run, expect FAIL.** Add to `CMakeLists.txt` (after `src/core/session_history.cpp` and the test block respectively):
```cmake
target_sources(hades_core PRIVATE src/embedding/vec_math.cpp)
target_sources(hades_tests PRIVATE tests/test_embedding_vec_math.cpp)
```
Run `nix develop --command cmake --build build` → compile error (no `vec_math.h`).

- [ ] **Step 3: Implement.** `include/hades/embedding/defaults.h`:
```cpp
// include/hades/embedding/defaults.h — single source of embedding default constants.
#pragma once
#include <cstddef>
namespace hades {
inline constexpr std::size_t kDefaultEmbedTopN = 5;
inline constexpr float       kDefaultMinSimilarity = 0.25f;   // weak-match floor
inline constexpr std::size_t kDefaultEmbedBatch = 32;
inline constexpr double      kDefaultEmbedTimeoutS = 120.0;
inline constexpr double      kDefaultReindexIntervalS = 86400.0;  // daily; 0 = launch-only
}  // namespace hades
```
`include/hades/embedding/provider.h`:
```cpp
// include/hades/embedding/provider.h — text -> vectors. Two impls: warm subprocess / OpenAI-compat HTTP.
//
// embed() returns one vector per input (same order). On ANY failure it returns a result with a
// non-empty `error` (NOT a throw) — every caller fail-softs (index skips, query returns empty).
#pragma once
#include <string>
#include <vector>
namespace hades {
struct EmbedResult {
  std::vector<std::vector<float>> vectors;  // one per input text, in order
  std::string model;                        // model id that produced these (stamped into the cache)
  int dim = 0;
  std::string error;                        // non-empty => failure; caller MUST fail-soft
};
class EmbeddingProvider {
public:
  virtual ~EmbeddingProvider() = default;
  virtual EmbedResult embed(const std::vector<std::string>& texts) = 0;
  virtual std::string model() const = 0;    // the model id (for the cache stamp / mismatch check)
};
}  // namespace hades
```
`include/hades/embedding/vec_math.h`:
```cpp
// include/hades/embedding/vec_math.h — cosine primitives. Normalize once at store+query time so
// similarity is a plain dot product.
#pragma once
#include <vector>
namespace hades {
// L2-normalize in place. Returns false (and leaves v unchanged) if the norm is ~0 (degenerate).
bool l2_normalize(std::vector<float>& v);
// Dot product of two equal-length vectors (== cosine when both are normalized). Size mismatch -> 0.
float dot(const std::vector<float>& a, const std::vector<float>& b);
}  // namespace hades
```
`src/embedding/vec_math.cpp`:
```cpp
#include "hades/embedding/vec_math.h"
#include <cmath>
namespace hades {
bool l2_normalize(std::vector<float>& v) {
  double s = 0.0;
  for (float x : v) s += static_cast<double>(x) * x;
  if (s <= 1e-12) return false;            // zero / degenerate
  const float inv = static_cast<float>(1.0 / std::sqrt(s));
  for (float& x : v) x *= inv;
  return true;
}
float dot(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size()) return 0.0f;
  float s = 0.0f;
  for (std::size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];
  return s;
}
}  // namespace hades
```

- [ ] **Step 4: Build + test** `-R VecMath` PASS, then FULL suite (`nix develop --command ctest --test-dir build`) → **208** (204 + 4).
- [ ] **Step 5: Commit** `feat: EmbeddingProvider interface + vec_math (l2_normalize/dot) + embedding defaults`

---

## Task 2: HttpEmbeddingProvider (OpenAI-compat /embeddings)

**Files:** Create `include/hades/embedding/http_embedding_provider.h`, `src/embedding/http_embedding_provider.cpp`, `tests/test_http_embedding_provider.cpp`. Modify `CMakeLists.txt`. **Read first:** `src/llm/openai_compat_provider.cpp` (the HttpClient seam + json parse discipline), `include/hades/llm/http.h` (`HttpClient`/`HttpResponse`/`cpr_http`).

**Interfaces — Consumes:** `EmbeddingProvider` (T1), `HttpClient` (existing). **Produces:** `HttpEmbeddingProvider(endpoint, api_key, model, HttpClient)`.

- [ ] **Step 1: Failing tests** — `tests/test_http_embedding_provider.cpp`:
```cpp
#include <gtest/gtest.h>
#include "hades/embedding/http_embedding_provider.h"
using namespace hades;

TEST(HttpEmbeddingProvider, ParsesEmbeddingsInOrder) {
  std::string canned = R"({"data":[
    {"embedding":[1.0,0.0]},
    {"embedding":[0.0,1.0]}],"model":"m"})";
  HttpEmbeddingProvider p("https://x/v1", "k", "m",
    [&](const std::string& url, auto, const std::string& body) {
      EXPECT_NE(url.find("/embeddings"), std::string::npos);
      EXPECT_NE(body.find("\"input\""), std::string::npos);
      return HttpResponse{200, canned};
    });
  auto r = p.embed({"a", "b"});
  EXPECT_TRUE(r.error.empty());
  ASSERT_EQ(r.vectors.size(), 2u);
  EXPECT_EQ(r.dim, 2);
  EXPECT_FLOAT_EQ(r.vectors[0][0], 1.0f);
  EXPECT_FLOAT_EQ(r.vectors[1][1], 1.0f);
}
TEST(HttpEmbeddingProvider, NonOkStatusIsSoftError) {
  HttpEmbeddingProvider p("https://x/v1", "k", "m",
    [](auto, auto, auto) { return HttpResponse{500, "boom"}; });
  auto r = p.embed({"a"});
  EXPECT_FALSE(r.error.empty());               // soft error, NOT a throw
  EXPECT_TRUE(r.vectors.empty());
}
TEST(HttpEmbeddingProvider, MalformedJsonIsSoftError) {
  HttpEmbeddingProvider p("https://x/v1", "k", "m",
    [](auto, auto, auto) { return HttpResponse{200, "not json"}; });
  auto r = p.embed({"a"});
  EXPECT_FALSE(r.error.empty());
  EXPECT_TRUE(r.vectors.empty());
}
TEST(HttpEmbeddingProvider, CountMismatchIsSoftError) {
  HttpEmbeddingProvider p("https://x/v1", "k", "m",
    [](auto, auto, auto) { return HttpResponse{200, R"({"data":[{"embedding":[1.0]}]})"}; });
  auto r = p.embed({"a", "b"});                // asked 2, got 1
  EXPECT_FALSE(r.error.empty());
}
```

- [ ] **Step 2: Register + run, expect FAIL.** Add CMake lines:
```cmake
target_sources(hades_core PRIVATE src/embedding/http_embedding_provider.cpp)
target_sources(hades_tests PRIVATE tests/test_http_embedding_provider.cpp)
```

- [ ] **Step 3: Implement.** `include/hades/embedding/http_embedding_provider.h`:
```cpp
#pragma once
#include <string>
#include "hades/embedding/provider.h"
#include "hades/llm/http.h"
namespace hades {
class HttpEmbeddingProvider : public EmbeddingProvider {
public:
  HttpEmbeddingProvider(std::string endpoint, std::string api_key, std::string model, HttpClient http);
  EmbedResult embed(const std::vector<std::string>& texts) override;
  std::string model() const override { return model_; }
private:
  std::string endpoint_, key_, model_;
  HttpClient http_;
};
}  // namespace hades
```
`src/embedding/http_embedding_provider.cpp`:
```cpp
#include "hades/embedding/http_embedding_provider.h"
#include <nlohmann/json.hpp>
namespace hades {
HttpEmbeddingProvider::HttpEmbeddingProvider(std::string e, std::string k, std::string m, HttpClient h)
  : endpoint_(std::move(e)), key_(std::move(k)), model_(std::move(m)), http_(std::move(h)) {}

EmbedResult HttpEmbeddingProvider::embed(const std::vector<std::string>& texts) {
  EmbedResult out;
  out.model = model_;
  nlohmann::json body{{"model", model_}, {"input", texts}};
  HttpResponse resp;
  try {
    resp = http_(endpoint_ + "/embeddings",
                 {{"Authorization", "Bearer " + key_}, {"Content-Type", "application/json"}},
                 body.dump());
  } catch (...) { out.error = "embedding http call threw"; return out; }
  if (resp.status < 200 || resp.status >= 300) { out.error = "embedding http status " + std::to_string(resp.status); return out; }
  auto j = nlohmann::json::parse(resp.body, nullptr, false);
  if (j.is_discarded() || !j.contains("data") || !j["data"].is_array()) { out.error = "embedding response not parseable"; return out; }
  for (const auto& d : j["data"]) {
    if (!d.is_object() || !d.contains("embedding") || !d["embedding"].is_array()) { out.error = "embedding item malformed"; return {}; }
    std::vector<float> v;
    for (const auto& x : d["embedding"]) { if (!x.is_number()) { out.error = "embedding value not number"; return {}; } v.push_back(x.get<float>()); }
    out.vectors.push_back(std::move(v));
  }
  if (out.vectors.size() != texts.size()) { return EmbedResult{{}, model_, 0, "embedding count mismatch"}; }
  out.dim = out.vectors.empty() ? 0 : static_cast<int>(out.vectors.front().size());
  for (const auto& v : out.vectors) if (static_cast<int>(v.size()) != out.dim) { return EmbedResult{{}, model_, 0, "embedding dim inconsistent"}; }
  return out;
}
}  // namespace hades
```

- [ ] **Step 4: Build + test** `-R HttpEmbeddingProvider` PASS, then FULL suite → **212** (208 + 4).
- [ ] **Step 5: Commit** `feat: HttpEmbeddingProvider (OpenAI-compat /embeddings, injected HttpClient, fail-soft)`

---

## Task 3: SubprocessEmbeddingProvider (warm child, line protocol, mutex-serialized)

**Files:** Create `include/hades/embedding/persistent_child.h`, `src/embedding/persistent_child.cpp`, `include/hades/embedding/subprocess_embedding_provider.h`, `src/embedding/subprocess_embedding_provider.cpp`, `tests/test_subprocess_embedding_provider.cpp`, `tests/fixtures/echo_embedder.py`. Modify `CMakeLists.txt`. **Read first:** `src/tool/subprocess.cpp` (fork/exec/pipe/poll/SIGKILL reference), `include/hades/tool/subprocess.h`.

**Interfaces — Consumes:** `EmbeddingProvider` (T1). **Produces:** `SubprocessEmbeddingProvider(std::vector<std::string> argv, double timeout_s)`; `PersistentChild` (start/request/alive/stop).

This is the most intricate task — a long-lived bidirectional child. `PersistentChild` keeps stdin/stdout pipes open across calls; `request(line)` writes one line and reads one reply line with a wall-clock timeout; on EOF/timeout it marks itself dead (next `request` restarts).

- [ ] **Step 1: Fixture** — `tests/fixtures/echo_embedder.py` (a deterministic warm embedder honoring the protocol; no ML deps):
```python
#!/usr/bin/env python3
import sys, json
# Warm process: load once (nothing here), then loop one request/response per line.
DIM = 3
def embed(text):                      # deterministic pseudo-embedding from char codes
    v = [0.0] * DIM
    for i, ch in enumerate(text):
        v[i % DIM] += (ord(ch) % 17) / 17.0
    return v
for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    try:
        req = json.loads(line)
        texts = req.get("texts", [])
        out = {"model": "echo", "dim": DIM, "embeddings": [embed(t) for t in texts]}
    except Exception as e:             # protocol error -> error line (still one line)
        out = {"error": str(e)}
    sys.stdout.write(json.dumps(out) + "\n")
    sys.stdout.flush()                 # CRITICAL: flush so hades can read the reply
```

- [ ] **Step 2: Failing tests** — `tests/test_subprocess_embedding_provider.cpp` (CMake passes the fixture path as `ECHO_EMBEDDER`):
```cpp
#include <gtest/gtest.h>
#include "hades/embedding/subprocess_embedding_provider.h"
using namespace hades;

TEST(SubprocessEmbeddingProvider, EmbedsBatchOverWarmChild) {
  SubprocessEmbeddingProvider p({"python3", ECHO_EMBEDDER}, 10.0);
  auto r = p.embed({"hello", "world"});
  EXPECT_TRUE(r.error.empty());
  ASSERT_EQ(r.vectors.size(), 2u);
  EXPECT_EQ(r.dim, 3);
  EXPECT_EQ(r.model, "echo");
}
TEST(SubprocessEmbeddingProvider, SecondCallReusesSameWarmChild) {
  SubprocessEmbeddingProvider p({"python3", ECHO_EMBEDDER}, 10.0);
  auto r1 = p.embed({"a"});
  auto r2 = p.embed({"b", "c"});               // same process, two requests
  EXPECT_TRUE(r1.error.empty());
  EXPECT_TRUE(r2.error.empty());
  ASSERT_EQ(r2.vectors.size(), 2u);
}
TEST(SubprocessEmbeddingProvider, MissingBinaryIsSoftError) {
  SubprocessEmbeddingProvider p({"/no/such/embedder"}, 5.0);
  auto r = p.embed({"a"});
  EXPECT_FALSE(r.error.empty());               // never throws
  EXPECT_TRUE(r.vectors.empty());
}
```

- [ ] **Step 3: Register + run, expect FAIL.** Add CMake:
```cmake
target_sources(hades_core PRIVATE src/embedding/persistent_child.cpp src/embedding/subprocess_embedding_provider.cpp)
target_sources(hades_tests PRIVATE tests/test_subprocess_embedding_provider.cpp)
target_compile_definitions(hades_tests PRIVATE ECHO_EMBEDDER="${CMAKE_SOURCE_DIR}/tests/fixtures/echo_embedder.py")
```

- [ ] **Step 4: Implement `PersistentChild`.** `include/hades/embedding/persistent_child.h`:
```cpp
// include/hades/embedding/persistent_child.h — a long-lived child process spoken to over stdin/stdout,
// ONE request line -> ONE response line, with a wall-clock timeout. Unlike run_subprocess (one-shot),
// the pipes stay open across calls so a warm model is loaded once. NOT thread-safe — the caller
// serializes (the embedding provider holds a mutex).
#pragma once
#include <string>
#include <vector>
namespace hades {
class PersistentChild {
public:
  PersistentChild(std::vector<std::string> argv, double timeout_s);
  ~PersistentChild();
  PersistentChild(const PersistentChild&) = delete;
  PersistentChild& operator=(const PersistentChild&) = delete;
  // Send one line (a "\n" is appended) and read one reply line. ok=false on spawn failure, write
  // failure, timeout, or EOF (the child is then marked dead; the next call respawns). Never throws.
  struct Reply { bool ok; std::string line; std::string err; };
  Reply request(const std::string& line);
private:
  bool ensure_started_();   // (re)spawn if not alive; false on spawn failure
  void stop_();             // close pipes, SIGKILL + reap if alive
  std::vector<std::string> argv_;
  double timeout_s_;
  int in_fd_ = -1;          // write to child stdin
  int out_fd_ = -1;         // read from child stdout
  int pid_ = -1;
  std::string rbuf_;        // carry-over bytes read past one '\n'
  bool alive_ = false;
};
}  // namespace hades
```
`src/embedding/persistent_child.cpp`:
```cpp
#include "hades/embedding/persistent_child.h"
#include <csignal>
#include <ctime>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
namespace hades {
namespace { double mono() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; } }

PersistentChild::PersistentChild(std::vector<std::string> argv, double timeout_s)
  : argv_(std::move(argv)), timeout_s_(timeout_s) {}
PersistentChild::~PersistentChild() { stop_(); }

bool PersistentChild::ensure_started_() {
  if (alive_) return true;
  int ip[2], op[2];
  if (pipe(ip) != 0) return false;
  if (pipe(op) != 0) { close(ip[0]); close(ip[1]); return false; }
  pid_t pid = fork();
  if (pid < 0) { close(ip[0]); close(ip[1]); close(op[0]); close(op[1]); return false; }
  if (pid == 0) {                          // child
    dup2(ip[0], 0); dup2(op[1], 1);
    for (int fd : {ip[0], ip[1], op[0], op[1]}) close(fd);
    std::vector<char*> a;
    for (auto& s : argv_) a.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr);
    execvp(a[0], a.data());
    _exit(127);                            // exec failed
  }
  close(ip[0]); close(op[1]);
  in_fd_ = ip[1]; out_fd_ = op[0]; pid_ = pid; alive_ = true; rbuf_.clear();
  return true;
}

void PersistentChild::stop_() {
  if (in_fd_ >= 0) { close(in_fd_); in_fd_ = -1; }
  if (out_fd_ >= 0) { close(out_fd_); out_fd_ = -1; }
  if (pid_ > 0) { kill(pid_, SIGKILL); int st = 0; waitpid(pid_, &st, 0); pid_ = -1; }
  alive_ = false;
}

PersistentChild::Reply PersistentChild::request(const std::string& line) {
  if (!ensure_started_()) return {false, "", "embedder spawn failed"};
  // write request + newline
  std::string msg = line; msg.push_back('\n');
  std::size_t off = 0;
  while (off < msg.size()) {
    ssize_t w = write(in_fd_, msg.data() + off, msg.size() - off);
    if (w <= 0) { stop_(); return {false, "", "embedder write failed"}; }
    off += static_cast<std::size_t>(w);
  }
  // read until we have one full line (or timeout / EOF)
  const double deadline = mono() + timeout_s_;
  for (;;) {
    if (auto nl = rbuf_.find('\n'); nl != std::string::npos) {
      std::string out = rbuf_.substr(0, nl);
      rbuf_.erase(0, nl + 1);
      return {true, out, ""};
    }
    double ms = (deadline - mono()) * 1000.0;
    if (ms <= 0) { stop_(); return {false, "", "embedder timed out"}; }
    pollfd pfd{out_fd_, POLLIN, 0};
    int n = poll(&pfd, 1, static_cast<int>(ms));
    if (n < 0) { stop_(); return {false, "", "embedder poll error"}; }
    if (n == 0) { stop_(); return {false, "", "embedder timed out"}; }
    char buf[4096];
    ssize_t k = read(out_fd_, buf, sizeof buf);
    if (k <= 0) { stop_(); return {false, "", "embedder closed (EOF)"}; }  // child died
    rbuf_.append(buf, static_cast<std::size_t>(k));
  }
}
}  // namespace hades
```

- [ ] **Step 5: Implement the provider** (parse + validate the reply, mutex-serialize). `include/hades/embedding/subprocess_embedding_provider.h`:
```cpp
#pragma once
#include <mutex>
#include <string>
#include <vector>
#include "hades/embedding/persistent_child.h"
#include "hades/embedding/provider.h"
namespace hades {
class SubprocessEmbeddingProvider : public EmbeddingProvider {
public:
  SubprocessEmbeddingProvider(std::vector<std::string> argv, double timeout_s);
  EmbedResult embed(const std::vector<std::string>& texts) override;  // mutex-serialized
  std::string model() const override;
private:
  PersistentChild child_;
  std::mutex mu_;                 // the warm child is shared (index task + per-turn query)
  std::string model_;             // learned from the first successful reply
};
}  // namespace hades
```
`src/embedding/subprocess_embedding_provider.cpp`:
```cpp
#include "hades/embedding/subprocess_embedding_provider.h"
#include <nlohmann/json.hpp>
namespace hades {
SubprocessEmbeddingProvider::SubprocessEmbeddingProvider(std::vector<std::string> argv, double timeout_s)
  : child_(std::move(argv), timeout_s) {}

std::string SubprocessEmbeddingProvider::model() const { return model_; }

EmbedResult SubprocessEmbeddingProvider::embed(const std::vector<std::string>& texts) {
  std::lock_guard<std::mutex> lk(mu_);          // serialize the shared warm child
  EmbedResult out;
  nlohmann::json req{{"texts", texts}};
  auto rep = child_.request(req.dump());
  if (!rep.ok) { out.error = rep.err; return out; }
  auto j = nlohmann::json::parse(rep.line, nullptr, false);
  if (j.is_discarded() || !j.is_object()) { out.error = "embedder reply not json object"; return out; }
  if (j.contains("error")) { out.error = "embedder error: " + j.value("error", std::string{"?"}); return out; }
  if (!j.contains("embeddings") || !j["embeddings"].is_array()) { out.error = "embedder reply missing embeddings"; return out; }
  out.model = j.value("model", std::string{});
  out.dim = j.value("dim", 0);
  for (const auto& row : j["embeddings"]) {
    if (!row.is_array()) { out.error = "embedder row not array"; return EmbedResult{}; }
    std::vector<float> v;
    for (const auto& x : row) { if (!x.is_number()) { out.error = "embedder value not number"; return EmbedResult{}; } v.push_back(x.get<float>()); }
    out.vectors.push_back(std::move(v));
  }
  if (out.vectors.size() != texts.size()) { return EmbedResult{{}, out.model, 0, "embedder count mismatch"}; }
  if (out.dim <= 0 && !out.vectors.empty()) out.dim = static_cast<int>(out.vectors.front().size());
  for (const auto& v : out.vectors) if (static_cast<int>(v.size()) != out.dim) { return EmbedResult{{}, out.model, 0, "embedder dim inconsistent"}; }
  if (!out.model.empty()) model_ = out.model;
  return out;
}
}  // namespace hades
```

- [ ] **Step 6: Build + test** `-R SubprocessEmbeddingProvider` PASS (the echo fixture must be executable: `python3 <path>` is invoked, so no chmod needed). Then FULL suite → **215** (212 + 3).
- [ ] **Step 7: Commit** `feat: SubprocessEmbeddingProvider over a warm PersistentChild (line protocol, mutex-serialized, fail-soft)`

---

## Task 4: VectorCache (model-stamped persistence + cosine retrieval)

**Files:** Create `include/hades/embedding/vector_cache.h`, `src/embedding/vector_cache.cpp`, `tests/test_vector_cache.cpp`. Modify `CMakeLists.txt`. **Read first:** `src/core/session_history.cpp` (tolerant jsonl read pattern), `src/embedding/vec_math.cpp` (T1).

**Interfaces — Consumes:** `vec_math` (T1). **Produces:** `CachedVec`, `ScoredMemory`, `VectorCache(path, model, dim)` with `load() -> bool`, `has(id)`, `ids()`, `put(CachedVec)`, `query(q, top_n, min_sim) -> vector<ScoredMemory>`, `size()`, `clear_file()`.

- [ ] **Step 1: Failing tests** — `tests/test_vector_cache.cpp`:
```cpp
#include <gtest/gtest.h>
#include <string>
#include "hades/embedding/vector_cache.h"
using namespace hades;

static std::string tmp(const std::string& n) { return testing::TempDir() + "/" + n; }

TEST(VectorCache, PutLoadRoundTripAndHas) {
  std::string p = tmp("vc_rt.jsonl");
  std::remove(p.c_str());
  { VectorCache c(p, "echo", 2); ASSERT_TRUE(c.load());
    c.put({"memory#0", "memory", "alpha", {1.0f, 0.0f}});
    c.put({"memory#1", "memory", "beta",  {0.0f, 1.0f}}); }
  VectorCache c2(p, "echo", 2);
  ASSERT_TRUE(c2.load());
  EXPECT_EQ(c2.size(), 2u);
  EXPECT_TRUE(c2.has("memory#0"));
  EXPECT_FALSE(c2.has("memory#99"));
}
TEST(VectorCache, QueryRanksByCosineAboveFloor) {
  std::string p = tmp("vc_q.jsonl");
  std::remove(p.c_str());
  VectorCache c(p, "echo", 2); ASSERT_TRUE(c.load());
  c.put({"a", "memory", "alpha", {1.0f, 0.0f}});
  c.put({"b", "memory", "beta",  {0.0f, 1.0f}});
  auto top = c.query({0.9f, 0.1f}, 5, 0.2f);   // closest to "alpha"
  ASSERT_FALSE(top.empty());
  EXPECT_EQ(top[0].text, "alpha");
  // a near-orthogonal query under the floor returns nothing
  EXPECT_TRUE(c.query({0.0f, 0.01f}, 5, 0.9f).empty());
}
TEST(VectorCache, ModelMismatchFailsLoadForRebuild) {
  std::string p = tmp("vc_mm.jsonl");
  std::remove(p.c_str());
  { VectorCache c(p, "echo", 2); ASSERT_TRUE(c.load()); c.put({"x", "memory", "t", {1.0f, 0.0f}}); }
  VectorCache other(p, "DIFFERENT-MODEL", 2);
  EXPECT_FALSE(other.load());                   // stamp mismatch -> caller rebuilds
}
TEST(VectorCache, MissingFileLoadsEmpty) {
  VectorCache c(tmp("vc_none.jsonl"), "echo", 2);
  EXPECT_TRUE(c.load());
  EXPECT_EQ(c.size(), 0u);
}
TEST(VectorCache, DegenerateZeroVectorDropped) {
  std::string p = tmp("vc_zero.jsonl");
  std::remove(p.c_str());
  VectorCache c(p, "echo", 2); ASSERT_TRUE(c.load());
  c.put({"z", "memory", "zero", {0.0f, 0.0f}});  // normalize fails -> dropped
  EXPECT_EQ(c.size(), 0u);
}
```

- [ ] **Step 2: Register + run, expect FAIL.** CMake:
```cmake
target_sources(hades_core PRIVATE src/embedding/vector_cache.cpp)
target_sources(hades_tests PRIVATE tests/test_vector_cache.cpp)
```

- [ ] **Step 3: Implement.** `include/hades/embedding/vector_cache.h`:
```cpp
// include/hades/embedding/vector_cache.h — model-stamped on-disk vector store + in-memory cosine query.
//
// One jsonl line per record {"id","src","model","dim","text","vec":[..]}. Vectors are L2-normalized
// at put() (a zero vector is dropped). load() returns FALSE if the file's stamped model/dim differ
// from the expected (model,dim) -> the caller rebuilds (clear_file + re-embed): comparing vectors
// from different models is garbage. Tolerant of blank/corrupt/partial lines.
#pragma once
#include <cstddef>
#include <map>
#include <string>
#include <vector>
namespace hades {
struct CachedVec { std::string id; std::string src; std::string text; std::vector<float> vec; };
struct ScoredMemory { std::string text; float score; };
class VectorCache {
public:
  VectorCache(std::string path, std::string model, int dim);
  bool load();                                   // false => model/dim mismatch (rebuild)
  bool has(const std::string& id) const;
  std::vector<std::string> ids() const;
  void put(const CachedVec& rec);                // normalizes; drops degenerate; appends a line
  std::vector<ScoredMemory> query(std::vector<float> q, std::size_t top_n, float min_similarity) const;
  std::size_t size() const { return mem_.size(); }
  void clear_file();                             // truncate the file + in-memory (rebuild path)
private:
  struct Rec { std::string text; std::string src; std::vector<float> vec; };
  std::string path_, model_;
  int dim_;
  std::map<std::string, Rec> mem_;               // id -> record (normalized vec)
};
}  // namespace hades
```
`src/embedding/vector_cache.cpp`:
```cpp
#include "hades/embedding/vector_cache.h"
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include "hades/embedding/vec_math.h"
namespace hades {
VectorCache::VectorCache(std::string path, std::string model, int dim)
  : path_(std::move(path)), model_(std::move(model)), dim_(dim) {}

bool VectorCache::load() {
  mem_.clear();
  std::ifstream f(path_);
  if (!f) return true;                           // missing -> empty, not a mismatch
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    auto j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) continue;
    if (j.value("model", std::string{}) != model_ || j.value("dim", -1) != dim_) {
      mem_.clear(); return false;                // stamp mismatch -> caller rebuilds
    }
    if (!j.contains("vec") || !j["vec"].is_array()) continue;
    std::vector<float> v;
    bool ok = true;
    for (const auto& x : j["vec"]) { if (!x.is_number()) { ok = false; break; } v.push_back(x.get<float>()); }
    if (!ok || static_cast<int>(v.size()) != dim_) continue;
    mem_[j.value("id", std::string{})] = {j.value("text", std::string{}), j.value("src", std::string{}), std::move(v)};
  }
  return true;
}

bool VectorCache::has(const std::string& id) const { return mem_.count(id) > 0; }
std::vector<std::string> VectorCache::ids() const {
  std::vector<std::string> r; r.reserve(mem_.size());
  for (const auto& [id, _] : mem_) r.push_back(id);
  return r;
}

void VectorCache::put(const CachedVec& rec) {
  std::vector<float> v = rec.vec;
  if (!l2_normalize(v)) return;                  // degenerate -> drop
  if (dim_ <= 0) dim_ = static_cast<int>(v.size());
  if (static_cast<int>(v.size()) != dim_) return;
  mem_[rec.id] = {rec.text, rec.src, v};
  std::ofstream f(path_, std::ios::app);
  if (!f) return;                                // disk hiccup: in-memory still has it
  nlohmann::json j{{"id", rec.id}, {"src", rec.src}, {"model", model_}, {"dim", dim_}, {"text", rec.text}, {"vec", v}};
  f << j.dump() << "\n";
}

std::vector<ScoredMemory> VectorCache::query(std::vector<float> q, std::size_t top_n, float min_similarity) const {
  if (!l2_normalize(q)) return {};
  std::vector<ScoredMemory> scored;
  for (const auto& [id, r] : mem_) {
    float s = dot(q, r.vec);
    if (s >= min_similarity) scored.push_back({r.text, s});
  }
  std::sort(scored.begin(), scored.end(), [](const ScoredMemory& a, const ScoredMemory& b) { return a.score > b.score; });
  if (scored.size() > top_n) scored.resize(top_n);
  return scored;
}

void VectorCache::clear_file() {
  mem_.clear();
  std::ofstream f(path_, std::ios::trunc);       // truncate
}
}  // namespace hades
```

- [ ] **Step 4: Build + test** `-R VectorCache` PASS, then FULL suite → **220** (215 + 5).
- [ ] **Step 5: Commit** `feat: VectorCache (model-stamped jsonl, normalized, tolerant, cosine query with floor)`

---

## Task 5: Indexer (incremental, archival corpus)

**Files:** Create `include/hades/embedding/indexer.h`, `src/embedding/indexer.cpp`, `tests/test_indexer.cpp`. Modify `CMakeLists.txt`. **Read first:** `include/hades/memory/store.h` (`load_memories`), `include/hades/memory/record.h`, `src/embedding/vector_cache.cpp` (T4), `include/hades/embedding/provider.h` (T1), `include/hades/embedding/defaults.h`.

**Interfaces — Consumes:** `EmbeddingProvider`, `VectorCache`, `load_memories`, defaults. **Produces:** `IndexStats index_archival(EmbeddingProvider&, VectorCache&, const std::string& memory_store, std::size_t batch_size)`. `IndexStats{ std::size_t embedded; std::size_t skipped; bool ok; }`. Behavior: for each archival record `i`, id = `"memory#" + i`; embed only records whose id is NOT in the cache; on a provider error, stop and return `ok=false` with what was embedded so far (fail-soft).

- [ ] **Step 1: Failing tests** — `tests/test_indexer.cpp`:
```cpp
#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include "hades/embedding/indexer.h"
#include "hades/embedding/provider.h"
#include "hades/embedding/vector_cache.h"
using namespace hades;

namespace {
struct FakeProvider : EmbeddingProvider {     // deterministic dim-2 vectors, counts calls
  int batches = 0; int texts = 0;
  EmbedResult embed(const std::vector<std::string>& in) override {
    ++batches; texts += static_cast<int>(in.size());
    EmbedResult r; r.model = "fake"; r.dim = 2;
    for (std::size_t i = 0; i < in.size(); ++i) r.vectors.push_back({static_cast<float>(in[i].size()), 1.0f});
    return r;
  }
  std::string model() const override { return "fake"; }
};
std::string tmp(const std::string& n) { return testing::TempDir() + "/" + n; }
void write_store(const std::string& p, int n) {
  std::ofstream f(p, std::ios::trunc);
  for (int i = 0; i < n; ++i) f << "{\"text\":\"fact " << i << "\",\"ts\":" << i << "}\n";
}
}  // namespace

TEST(Indexer, EmbedsAllNewRecords) {
  std::string store = tmp("ix_store.jsonl"), cache = tmp("ix_cache.jsonl");
  std::remove(cache.c_str());
  write_store(store, 3);
  FakeProvider prov;
  VectorCache vc(cache, "fake", 2); ASSERT_TRUE(vc.load());
  auto st = index_archival(prov, vc, store, 32);
  EXPECT_TRUE(st.ok);
  EXPECT_EQ(st.embedded, 3u);
  EXPECT_EQ(vc.size(), 3u);
  EXPECT_TRUE(vc.has("memory#0"));
}
TEST(Indexer, IncrementalSkipsAlreadyCached) {
  std::string store = tmp("ix_store2.jsonl"), cache = tmp("ix_cache2.jsonl");
  std::remove(cache.c_str());
  write_store(store, 2);
  { FakeProvider p1; VectorCache vc(cache, "fake", 2); vc.load(); index_archival(p1, vc, store, 32); }
  write_store(store, 4);                        // 2 new records appended
  FakeProvider p2;
  VectorCache vc(cache, "fake", 2); ASSERT_TRUE(vc.load());
  auto st = index_archival(p2, vc, store, 32);
  EXPECT_EQ(st.embedded, 2u);                   // only the 2 NEW ones
  EXPECT_EQ(st.skipped, 2u);
  EXPECT_EQ(p2.texts, 2);                       // provider asked only for the new ones
  EXPECT_EQ(vc.size(), 4u);
}
```

- [ ] **Step 2: Register + run, expect FAIL.** CMake:
```cmake
target_sources(hades_core PRIVATE src/embedding/indexer.cpp)
target_sources(hades_tests PRIVATE tests/test_indexer.cpp)
```

- [ ] **Step 3: Implement.** `include/hades/embedding/indexer.h`:
```cpp
// include/hades/embedding/indexer.h — incremental embedding of a corpus into a VectorCache.
//
// index_archival: each memory.jsonl record i has stable id "memory#i". Only ids absent from the
// cache are embedded (in batch_size batches); a provider error stops early and returns ok=false
// with whatever was embedded (fail-soft — the module keeps running, keyword still answers).
#pragma once
#include <cstddef>
#include <string>
namespace hades {
class EmbeddingProvider;
class VectorCache;
struct IndexStats { std::size_t embedded = 0; std::size_t skipped = 0; bool ok = true; };
IndexStats index_archival(EmbeddingProvider& provider, VectorCache& cache,
                          const std::string& memory_store, std::size_t batch_size);
}  // namespace hades
```
`src/embedding/indexer.cpp`:
```cpp
#include "hades/embedding/indexer.h"
#include <string>
#include <vector>
#include "hades/embedding/provider.h"
#include "hades/embedding/vector_cache.h"
#include "hades/memory/store.h"
namespace hades {
namespace {
// Embed a batch of (id,text) into the cache. Returns false on provider error (fail-soft stop).
bool flush_batch(EmbeddingProvider& provider, VectorCache& cache,
                 std::vector<std::pair<std::string, std::string>>& batch, IndexStats& st) {
  if (batch.empty()) return true;
  std::vector<std::string> texts;
  texts.reserve(batch.size());
  for (auto& [id, text] : batch) texts.push_back(text);
  EmbedResult r = provider.embed(texts);
  if (!r.error.empty() || r.vectors.size() != batch.size()) { st.ok = false; batch.clear(); return false; }
  for (std::size_t i = 0; i < batch.size(); ++i) {
    cache.put({batch[i].first, "memory", batch[i].second, r.vectors[i]});
    ++st.embedded;
  }
  batch.clear();
  return true;
}
}  // namespace

IndexStats index_archival(EmbeddingProvider& provider, VectorCache& cache,
                          const std::string& memory_store, std::size_t batch_size) {
  IndexStats st;
  const auto records = load_memories(memory_store);
  if (batch_size == 0) batch_size = 1;
  std::vector<std::pair<std::string, std::string>> batch;
  for (std::size_t i = 0; i < records.size(); ++i) {
    const std::string id = "memory#" + std::to_string(i);
    if (cache.has(id)) { ++st.skipped; continue; }
    if (records[i].text.empty()) { ++st.skipped; continue; }
    batch.emplace_back(id, records[i].text);
    if (batch.size() >= batch_size && !flush_batch(provider, cache, batch, st)) return st;
  }
  flush_batch(provider, cache, batch, st);
  return st;
}
}  // namespace hades
```

- [ ] **Step 4: Build + test** `-R Indexer` PASS, then FULL suite → **222** (220 + 2).
- [ ] **Step 5: Commit** `feat: incremental archival indexer (stable memory#i ids, batched, fail-soft)`

---

## Task 6: EmbeddingMemoryModule (query path, provider factory, inline index)

**Files:** Create `include/hades/module/embedding_memory_module.h`, `src/module/embedding_memory_module.cpp`, `tests/test_embedding_memory_module.cpp`. Modify `CMakeLists.txt`. **Read first:** `src/module/memory_module.cpp` + `.h` (the model this mirrors), `src/module/llm_module.cpp` (injected-provider ctor + `set_executor` pattern), `include/hades/blackboard.h`, `include/hades/config.h` (`set_pos_double_on_string`), `include/hades/executor.h`, all of T1–T5.

**Interfaces — Consumes:** everything T1–T5; `Executor`. **Produces:** `EmbeddingMemoryModule` (`type()=="embedding_memory"`), test ctor `EmbeddingMemoryModule(std::unique_ptr<EmbeddingProvider>)`, `void set_executor(Executor*)`. Posts `RETRIEVED_MEMORY_SEMANTIC`.

- [ ] **Step 1: Failing tests** — `tests/test_embedding_memory_module.cpp` (inject a fake provider; NO executor → index runs inline):
```cpp
#include <gtest/gtest.h>
#include <fstream>
#include <memory>
#include <string>
#include "hades/module/embedding_memory_module.h"
#include "hades/blackboard.h"
#include "hades/config.h"
using namespace hades;

namespace {
struct FakeProvider : EmbeddingProvider {       // "alpha"->(1,0), "beta"->(0,1), query echoes
  EmbedResult embed(const std::vector<std::string>& in) override {
    EmbedResult r; r.model = "fake"; r.dim = 2;
    for (const auto& t : in) {
      if (t.find("alpha") != std::string::npos) r.vectors.push_back({1.0f, 0.0f});
      else if (t.find("beta") != std::string::npos) r.vectors.push_back({0.0f, 1.0f});
      else r.vectors.push_back({1.0f, 0.0f});   // default near alpha
    }
    return r;
  }
  std::string model() const override { return "fake"; }
};
struct FailProvider : EmbeddingProvider {
  EmbedResult embed(const std::vector<std::string>&) override { return {{}, "fake", 0, "boom"}; }
  std::string model() const override { return "fake"; }
};
std::string tmp(const std::string& n) { return testing::TempDir() + "/" + n; }
Block cfg(const std::string& store, const std::string& cache) {
  Block b; b.section = "Embedding";
  b.kv["memory_store"] = store; b.kv["cache_dir"] = cache; b.kv["min_similarity"] = "0.2";
  b.kv["index_sessions"] = "false";
  return b;
}
}  // namespace

TEST(EmbeddingMemoryModule, RetrievesSemanticMatchAndPosts) {
  std::string store = tmp("em_store.jsonl"), cache = tmp("em_cache");
  { std::ofstream f(store, std::ios::trunc); f << "{\"text\":\"alpha fact\",\"ts\":1}\n{\"text\":\"beta fact\",\"ts\":2}\n"; }
  std::remove((cache + "/memory.vec.jsonl").c_str());
  Blackboard bb;
  EmbeddingMemoryModule m(std::make_unique<FakeProvider>());
  m.on_start(cfg(store, cache), bb);            // inline index (no executor)
  m.on_attach(bb);
  std::string got;
  bb.subscribe("RETRIEVED_MEMORY_SEMANTIC", [&](const Entry& e) { got = e.value.get<std::string>(); });
  bb.post("USER_MESSAGE", "tell me about alpha", "chat");
  bb.pump();
  EXPECT_NE(got.find("alpha fact"), std::string::npos);
  EXPECT_EQ(got.find("beta fact"), std::string::npos);  // below floor for an alpha query
}
TEST(EmbeddingMemoryModule, ProviderFailureIsSoftEmpty) {
  std::string store = tmp("em_store2.jsonl"), cache = tmp("em_cache2");
  { std::ofstream f(store, std::ios::trunc); f << "{\"text\":\"alpha\",\"ts\":1}\n"; }
  std::remove((cache + "/memory.vec.jsonl").c_str());
  Blackboard bb;
  EmbeddingMemoryModule m(std::make_unique<FailProvider>());
  m.on_start(cfg(store, cache), bb);
  m.on_attach(bb);
  std::string got = "UNSET";
  bb.subscribe("RETRIEVED_MEMORY_SEMANTIC", [&](const Entry& e) { got = e.value.get<std::string>(); });
  bb.post("USER_MESSAGE", "alpha?", "chat");
  bb.pump();                                    // must NOT crash
  EXPECT_EQ(got, "");                            // fail-soft: empty
}
```

- [ ] **Step 2: Register + run, expect FAIL.** CMake:
```cmake
target_sources(hades_core PRIVATE src/module/embedding_memory_module.cpp)
target_sources(hades_tests PRIVATE tests/test_embedding_memory_module.cpp)
```

- [ ] **Step 3: Implement.** `include/hades/module/embedding_memory_module.h`:
```cpp
// include/hades/module/embedding_memory_module.h — opt-in semantic-memory app (MOOS-app style).
//
// Mirrors MemoryModule but ranks by embeddings: on USER_MESSAGE it embeds the query (warm provider),
// cosine-ranks the VectorCache above min_similarity, and posts RETRIEVED_MEMORY_SEMANTIC (the Arbiter
// merges it with the keyword RETRIEVED_MEMORY). Corpus is indexed incrementally: on an Executor worker
// when set (live), else inline (tests). Every embedder failure is fail-soft (empty result, no crash).
#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include "hades/embedding/defaults.h"
#include "hades/embedding/provider.h"
#include "hades/module.h"
namespace hades {
class Blackboard;
class Executor;
class EmbeddingMemoryModule : public Module {
public:
  EmbeddingMemoryModule() = default;
  explicit EmbeddingMemoryModule(std::unique_ptr<EmbeddingProvider> p);  // test injection
  std::string type() const override { return "embedding_memory"; }
  void on_start(const Block& cfg, Blackboard& bb) override;
  void on_attach(Blackboard& bb) override;
  void set_executor(Executor* ex) { executor_ = ex; }
private:
  void run_index_();                            // incremental index of the archival corpus
  std::unique_ptr<EmbeddingProvider> provider_;
  std::string memory_store_ = ".hades/memory.jsonl";
  std::string cache_dir_ = ".hades/embeddings";
  std::size_t top_n_ = kDefaultEmbedTopN;
  float min_similarity_ = kDefaultMinSimilarity;
  std::size_t batch_size_ = kDefaultEmbedBatch;
  double timeout_s_ = kDefaultEmbedTimeoutS;
  Blackboard* bb_ = nullptr;
  Executor* executor_ = nullptr;
};
}  // namespace hades
```
`src/module/embedding_memory_module.cpp` (provider factory from the Block lives here):
```cpp
#include "hades/module/embedding_memory_module.h"
#include <sstream>
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/executor.h"
#include "hades/embedding/http_embedding_provider.h"
#include "hades/embedding/indexer.h"
#include "hades/embedding/subprocess_embedding_provider.h"
#include "hades/embedding/vector_cache.h"
#include "hades/llm/http.h"
#include <cstdlib>
namespace hades {
namespace {
std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> v; std::istringstream is(s); std::string w; while (is >> w) v.push_back(w); return v;
}
std::string cache_path(const std::string& dir) { return dir + "/memory.vec.jsonl"; }
}  // namespace

EmbeddingMemoryModule::EmbeddingMemoryModule(std::unique_ptr<EmbeddingProvider> p) : provider_(std::move(p)) {}

void EmbeddingMemoryModule::on_start(const Block& cfg, Blackboard&) {
  if (cfg.kv.count("memory_store")) memory_store_ = cfg.kv.at("memory_store");
  if (cfg.kv.count("cache_dir")) cache_dir_ = cfg.kv.at("cache_dir");
  if (cfg.kv.count("top_n")) { try { long n = std::stol(cfg.kv.at("top_n")); if (n > 0) top_n_ = static_cast<std::size_t>(n); } catch (...) {} }
  if (cfg.kv.count("batch_size")) { try { long n = std::stol(cfg.kv.at("batch_size")); if (n > 0) batch_size_ = static_cast<std::size_t>(n); } catch (...) {} }
  if (cfg.kv.count("min_similarity")) { double d = min_similarity_; if (set_pos_double_on_string(cfg.kv.at("min_similarity"), d)) min_similarity_ = static_cast<float>(d); }
  if (cfg.kv.count("timeout_s")) { double d = timeout_s_; if (set_pos_double_on_string(cfg.kv.at("timeout_s"), d)) timeout_s_ = d; }
  if (!provider_) {                              // build from config (not the test-injected path)
    const std::string kind = cfg.kv.count("provider") ? cfg.kv.at("provider") : "subprocess";
    if (kind == "http") {
      const std::string ep = cfg.kv.count("endpoint") ? cfg.kv.at("endpoint") : "";
      const std::string model = cfg.kv.count("model") ? cfg.kv.at("model") : "";
      const std::string env = cfg.kv.count("api_key_env") ? cfg.kv.at("api_key_env") : "HADES_EMBED_KEY";
      const char* key = std::getenv(env.c_str());
      provider_ = std::make_unique<HttpEmbeddingProvider>(ep, key ? key : "", model, cpr_http(timeout_s_));
    } else {                                     // subprocess (default)
      const std::string cmd = cfg.kv.count("command") ? cfg.kv.at("command") : "";
      provider_ = std::make_unique<SubprocessEmbeddingProvider>(split_ws(cmd), timeout_s_);
    }
  }
}

void EmbeddingMemoryModule::run_index_() {
  if (!provider_ || !bb_) return;
  std::error_code ec;
  std::filesystem::create_directories(cache_dir_, ec);
  // Probe one embed to learn model+dim: a subprocess provider only knows them after the first reply,
  // and the cache must be stamped with the real (model,dim) so the per-turn query path can detect a
  // mismatch and rebuild rather than silently compare incomparable vectors.
  EmbedResult probe = provider_->embed({"_probe_"});
  if (!probe.error.empty()) { bb_->post("EMBED_INDEX_DONE", false, "embedding_memory"); return; }
  VectorCache vc(cache_path(cache_dir_), probe.model, probe.dim);
  if (!vc.load()) vc.clear_file();               // stamped model/dim changed -> rebuild from scratch
  index_archival(*provider_, vc, memory_store_, batch_size_);
  bb_->post("EMBED_INDEX_DONE", true, "embedding_memory");
}

void EmbeddingMemoryModule::on_attach(Blackboard& bb) {
  bb_ = &bb;
  if (executor_) executor_->submit([this] { run_index_(); });  // live: off the bus
  else run_index_();                                           // tests: inline + deterministic
  bb.subscribe("USER_MESSAGE", [this](const Entry& e) {
    if (!e.value.is_string() || !provider_) { bb_->post("RETRIEVED_MEMORY_SEMANTIC", "", "embedding_memory"); return; }
    EmbedResult q = provider_->embed({e.value.get<std::string>()});
    if (!q.error.empty() || q.vectors.size() != 1) { bb_->post("RETRIEVED_MEMORY_SEMANTIC", "", "embedding_memory"); return; }
    VectorCache vc(cache_path(cache_dir_), q.model, q.dim);
    if (!vc.load()) { bb_->post("RETRIEVED_MEMORY_SEMANTIC", "", "embedding_memory"); return; }  // mismatch -> keyword only
    auto top = vc.query(q.vectors[0], top_n_, min_similarity_);
    std::string rendered;
    for (const auto& r : top) rendered += "- " + r.text + "\n";
    if (!rendered.empty() && rendered.back() == '\n') rendered.pop_back();
    bb_->post("RETRIEVED_MEMORY_SEMANTIC", rendered, "embedding_memory");
  });
}
}  // namespace hades
```
**Step 3 note (implementer):** add `#include <filesystem>` and `#include <system_error>` to the .cpp. The query path **re-opens the cache read-only per turn** (cheap; the cache is small) — do NOT hold one shared mutable `VectorCache` across the index thread and the pump thread (that would be a data race). The index thread writes the cache file (append-only `put`); the per-turn query opens a fresh read each time and tolerates a half-written final line (the `VectorCache::load` tolerant parse).

- [ ] **Step 4: Build + test** `-R EmbeddingMemoryModule` PASS, then FULL suite → **224** (222 + 2).
- [ ] **Step 5: Commit** `feat: EmbeddingMemoryModule (semantic query path, provider factory, inline/executor index, fail-soft)`

---

## Task 7: Arbiter — merge + dedup RETRIEVED_MEMORY + RETRIEVED_MEMORY_SEMANTIC

**Files:** Modify `src/arbiter/arbiter.cpp` (the `start_turn` injection block, lines ~173-184). Extend `tests/test_arbiter.cpp`. **Read first:** `src/arbiter/arbiter.cpp:153-191`.

**Interfaces — Produces:** the injected "Relevant memories:" block is now the dedup-merge of both keys; semantic absent/empty → byte-identical to today.

- [ ] **Step 1: Failing tests** — append to `tests/test_arbiter.cpp`:
```cpp
TEST(Arbiter, MergesKeywordAndSemanticMemoryDeduped) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  std::vector<nlohmann::json> reqs;
  bb.subscribe("LLM_REQUEST", [&](const Entry& e) { reqs.push_back(e.value); });
  bb.post("RETRIEVED_MEMORY", "- shared fact\n- keyword only", "memory");
  bb.post("RETRIEVED_MEMORY_SEMANTIC", "- shared fact\n- semantic only", "embedding_memory");
  bb.post("USER_MESSAGE", "hi", "chat");
  bb.pump();
  ASSERT_FALSE(reqs.empty());
  // find the injected system "Relevant memories:" block
  std::string block;
  for (const auto& m : reqs[0]["messages"])
    if (m.value("role", "") == "system" && m.value("content", "").rfind("Relevant memories:", 0) == 0)
      block = m["content"];
  ASSERT_FALSE(block.empty());
  // shared fact appears once; both unique lines present
  EXPECT_NE(block.find("shared fact"), std::string::npos);
  EXPECT_NE(block.find("keyword only"), std::string::npos);
  EXPECT_NE(block.find("semantic only"), std::string::npos);
  EXPECT_EQ(block.find("shared fact"), block.rfind("shared fact"));  // exactly once
}
TEST(Arbiter, SemanticAbsentIsUnchanged) {
  Blackboard bb; Arbiter a; a.on_attach(bb);
  std::vector<nlohmann::json> reqs;
  bb.subscribe("LLM_REQUEST", [&](const Entry& e) { reqs.push_back(e.value); });
  bb.post("RETRIEVED_MEMORY", "- only keyword", "memory");
  bb.post("USER_MESSAGE", "hi", "chat");
  bb.pump();
  std::string block;
  for (const auto& m : reqs[0]["messages"])
    if (m.value("role", "") == "system" && m.value("content", "").rfind("Relevant memories:", 0) == 0)
      block = m["content"];
  EXPECT_EQ(block, "Relevant memories:\n- only keyword");  // identical to pre-embedding behavior
}
```

- [ ] **Step 2: Run, expect FAIL** (semantic key ignored today): `nix develop --command ctest --test-dir build -R Arbiter`.

- [ ] **Step 3: Implement.** In `src/arbiter/arbiter.cpp`, replace the injection block (currently lines ~173-184) with a merge. Add a file-local helper above `start_turn` (in the anonymous namespace if one exists, else a `static` function):
```cpp
// Merge two "- bullet\n" lists into one, de-duplicating identical lines, keyword order first.
static std::string merge_memory_blocks(const std::string& keyword, const std::string& semantic) {
  std::vector<std::string> lines;
  auto add = [&](const std::string& blk) {
    std::size_t pos = 0;
    while (pos < blk.size()) {
      std::size_t nl = blk.find('\n', pos);
      std::string line = blk.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      if (!line.empty() && std::find(lines.begin(), lines.end(), line) == lines.end()) lines.push_back(line);
      if (nl == std::string::npos) break;
      pos = nl + 1;
    }
  };
  add(keyword);
  add(semantic);
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) { out += lines[i]; if (i + 1 < lines.size()) out += "\n"; }
  return out;
}
```
(Add `#include <algorithm>` if not present.) Then replace the injection block:
```cpp
  // Inject dynamically retrieved memory (keyword RETRIEVED_MEMORY + semantic RETRIEVED_MEMORY_SEMANTIC,
  // merged + deduped) as an ephemeral {role:system} block immediately before the last user message.
  // Recomputed each turn, never stored in history_. Semantic absent/empty -> identical to keyword-only.
  std::string kw, sem;
  if (auto m = bb_->get("RETRIEVED_MEMORY"); m && m->value.is_string()) kw = m->value.get<std::string>();
  if (auto m = bb_->get("RETRIEVED_MEMORY_SEMANTIC"); m && m->value.is_string()) sem = m->value.get<std::string>();
  std::string merged = merge_memory_blocks(kw, sem);
  if (!merged.empty()) {
    nlohmann::json block = {{"role", "system"}, {"content", "Relevant memories:\n" + merged}};
    int last_user = -1;
    for (int i = 0; i < static_cast<int>(messages.size()); ++i)
      if (messages[i].value("role", "") == "user") last_user = i;
    if (last_user >= 0) messages.insert(messages.begin() + last_user, block);
  }
```

- [ ] **Step 4: Build + test** `-R Arbiter` PASS (incl. the existing memory-injection tests — semantic absent → unchanged), then FULL suite → **226** (224 + 2).
- [ ] **Step 5: Commit** `feat: Arbiter merges+dedups keyword + semantic retrieved memory into one injected block`

---

## Task 8: Wiring + manifest + reference embedder (P1 end-to-end)

**Files:** Modify `app/agent_wiring.h` (Agent struct + factory), `app/agent_wiring.cpp` (`take_as` + `wire_agent` + executor), `manifests/dev.hades` (commented opt-in example), `CMakeLists.txt`. Create `tools/embed_reference.py`, `tests/test_embedding_wiring.cpp`. Update `CLAUDE.md`. **Read first:** `app/agent_wiring.cpp:140-249` (the `wire_agent` memory-before-arbiter ordering + the Launcher factory registration + `take_as`), `app/agent_wiring.h` (Agent struct), `src/module/llm_module.cpp` (`set_executor` usage), `manifests/dev.hades`.

**Interfaces — Consumes:** `EmbeddingMemoryModule` (T6). **Produces:** `Agent.embedding`; the `embedding_memory` factory; wired before the Arbiter with the Executor set.

- [ ] **Step 1: Failing test** — `tests/test_embedding_wiring.cpp` (the Manifest path builds the module when rostered; absent → null):
```cpp
#include <gtest/gtest.h>
#include "app/agent_wiring.h"
#include "hades/blackboard.h"
#include "hades/config.h"
using namespace hades;

TEST(EmbeddingWiring, RosterWithEmbeddingMemoryBuildsModule) {
  const char* src = R"(
Session { provider = openai_compat
  endpoint = https://x/v1
  model = m
  api_key_env = HADES_API_KEY
}
Module = llm
Module = arbiter
Module = embedding_memory
Embedding { provider = subprocess
  command = /bin/true
  cache_dir = .hades/embeddings
  memory_store = .hades/memory.jsonl
  index_sessions = false
}
)";
  setenv("HADES_API_KEY", "k", 1);
  Manifest m = parse_manifest(src);
  Blackboard bb;
  Agent a = build_agent(bb, m);
  EXPECT_NE(a.embedding, nullptr);
}
TEST(EmbeddingWiring, RosterWithoutEmbeddingMemoryLeavesItNull) {
  const char* src = R"(
Session { provider = openai_compat
  endpoint = https://x/v1
  model = m
  api_key_env = HADES_API_KEY
}
Module = llm
Module = arbiter
)";
  setenv("HADES_API_KEY", "k", 1);
  Manifest m = parse_manifest(src);
  Blackboard bb;
  Agent a = build_agent(bb, m);
  EXPECT_EQ(a.embedding, nullptr);
}
```

- [ ] **Step 2: Register + run, expect FAIL.** CMake:
```cmake
target_sources(hades_tests PRIVATE tests/test_embedding_wiring.cpp)
```

- [ ] **Step 3: Implement wiring.**
  - `app/agent_wiring.h`: add `#include "hades/module/embedding_memory_module.h"` and a member to `Agent` (place it right after `memory`, before `chat`, so teardown order keeps it among the modules and before `executor`):
    ```cpp
    std::unique_ptr<MemoryModule> memory;
    std::unique_ptr<EmbeddingMemoryModule> embedding;   // optional semantic-memory app
    std::unique_ptr<ChatModule>   chat;
    ```
  - `app/agent_wiring.cpp`: register the factory + `take_as` it (alongside the others at lines 235-249):
    ```cpp
    launcher.register_factory("embedding_memory", []{ return std::make_unique<EmbeddingMemoryModule>(); });
    ```
    ```cpp
    a.embedding = take_as<EmbeddingMemoryModule>(launcher, "embedding_memory");
    ```
  - In `wire_agent` (the function that configures+attaches modules in order), wire the embedding module **before the Arbiter** (same reason MemoryModule is before the Arbiter — it must post `RETRIEVED_MEMORY_SEMANTIC` on the same pump before `start_turn` reads it). The Embedding block config comes from `m.of("Embedding")` (first block, or an empty Block). Null-guard `a.embedding` (roster may omit it). Set the executor so the index runs off the bus. Concretely, after the MemoryModule on_start/on_attach and before the Arbiter's:
    ```cpp
    if (a.embedding) {
      Block embed_cfg;                          // first Embedding block, or empty
      auto eb = m.of("Embedding");
      if (!eb.empty()) embed_cfg = eb.front();
      a.embedding->on_start(embed_cfg, bb);
      if (a.executor) a.embedding->set_executor(a.executor.get());
      a.embedding->on_attach(bb);               // subscribes USER_MESSAGE before the Arbiter does
    }
    ```
    (Find the exact analogous MemoryModule on_start/on_attach site in `wire_agent` and mirror it; the Manifest `m` and `bb` are in scope there. The executor is created on the Manifest path — confirm `a.executor` is set before this point, like it is for the LLMModule; if the executor is created later, move the embedding `set_executor` to just after the executor is constructed, but keep `on_attach` before the Arbiter's attach.)
  - **Test-overload safety:** the `build_agent(bb, provider, tools, objectives, model, memory, session)` overload does NOT build `a.embedding` (it stays null) — leave that overload untouched so the 226 existing tests are unaffected.

- [ ] **Step 4: Reference embedder** — `tools/embed_reference.py` (the known-good warm subprocess embedder):
```python
#!/usr/bin/env python3
"""hades reference embedder — a warm subprocess speaking the hades embedding line protocol.

Protocol (one JSON line in, one JSON line out, repeated for the process lifetime):
  in : {"texts": ["t1", "t2", ...]}
  out: {"model": "<name>", "dim": <int>, "embeddings": [[...], [...]]}
  err: {"error": "<msg>"}

Setup:  pip install sentence-transformers
Manifest: Embedding { provider = subprocess ; command = python3 /abs/path/tools/embed_reference.py }
"""
import sys, json
from sentence_transformers import SentenceTransformer   # loaded ONCE (warm)

MODEL_NAME = "all-MiniLM-L6-v2"   # 384-dim, small, CPU-fast
model = SentenceTransformer(MODEL_NAME)
DIM = model.get_sentence_embedding_dimension()

for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    try:
        req = json.loads(line)
        texts = req.get("texts", [])
        vecs = model.encode(texts, normalize_embeddings=False).tolist() if texts else []
        out = {"model": MODEL_NAME, "dim": DIM, "embeddings": vecs}
    except Exception as e:
        out = {"error": str(e)}
    sys.stdout.write(json.dumps(out) + "\n")
    sys.stdout.flush()   # CRITICAL: flush so hades reads the reply
```

- [ ] **Step 5: Manifest example** — append a **commented** opt-in block to `manifests/dev.hades` (keep dev keyword-by-default; do NOT add the `Module = embedding_memory` line uncommented, so dev.hades stays runnable without an embedder). Add as comments:
```
# --- Opt-in semantic memory (uncomment + point `command` at an embedder to enable) ---
# Module = embedding_memory
# Embedding {
#   provider           = subprocess
#   command            = python3 ./tools/embed_reference.py
#   cache_dir          = .hades/embeddings
#   memory_store       = .hades/memory.jsonl
#   sessions_dir       = .hades/sessions
#   index_sessions     = true
#   top_n              = 5
#   min_similarity     = 0.25
#   reindex_interval_s = 86400
#   batch_size         = 32
#   timeout_s          = 120
# }
```

- [ ] **Step 6: Build + FULL suite green** (228 = 226 + 2 wiring tests). Confirm `dev.hades` still builds + the existing manifest-lock tests pass (the commented block must not trip the multi-kv parser — comments are stripped before kv parsing; verify).
- [ ] **Step 7: Update `CLAUDE.md`** — add a short "Memory embeddings (P1)" subsection under the memory section (the opt-in `embedding_memory` module, warm subprocess/HTTP provider, model-stamped cache, hybrid merge, fail-soft, reference embedder) and bump the test count.
- [ ] **Step 8: Commit** `feat: wire embedding_memory module (roster + executor + before-arbiter) + reference embedder + manifest example`

---

# PHASE 2 — session corpus + periodic reindex

## Task 9: Session per-turn extraction (pure)

**Files:** Create `include/hades/embedding/session_turns.h`, `src/embedding/session_turns.cpp`, `tests/test_session_turns.cpp`. Modify `CMakeLists.txt`. **Read first:** `include/hades/session_history.h` (`read_session_jsonl`), `src/core/session_history.cpp`.

**Interfaces — Consumes:** `read_session_jsonl` (returns `std::vector<nlohmann::json>`). **Produces:** `struct SessionTurn { std::string id; std::string text; };` and `std::vector<SessionTurn> extract_session_turns(const std::string& session_file)` — pairs each `{role:user,content:string}` with the NEXT `{role:assistant,content:string}`; renders `"U: <user>\nA: <assistant>"`; id = `"session:<basename>#<turn-index>"`. Tool turns and tool_calls-assistants (content null) are skipped when pairing (they fold out — only a real user→assistant text pair forms a turn).

- [ ] **Step 1: Failing tests** — `tests/test_session_turns.cpp`:
```cpp
#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include "hades/embedding/session_turns.h"
using namespace hades;

static std::string write_sess(const std::string& name, const std::string& body) {
  std::string p = testing::TempDir() + "/" + name;
  std::ofstream f(p, std::ios::trunc); f << body; return p;
}

TEST(SessionTurns, PairsUserWithFollowingAssistant) {
  std::string p = write_sess("st1.jsonl",
    "{\"role\":\"user\",\"content\":\"q1\"}\n"
    "{\"role\":\"assistant\",\"content\":\"a1\"}\n"
    "{\"role\":\"user\",\"content\":\"q2\"}\n"
    "{\"role\":\"assistant\",\"content\":\"a2\"}\n");
  auto turns = extract_session_turns(p);
  ASSERT_EQ(turns.size(), 2u);
  EXPECT_NE(turns[0].text.find("U: q1"), std::string::npos);
  EXPECT_NE(turns[0].text.find("A: a1"), std::string::npos);
  EXPECT_NE(turns[0].id.find("#0"), std::string::npos);
  EXPECT_NE(turns[1].id.find("#1"), std::string::npos);
}
TEST(SessionTurns, FoldsToolTurnsAndSkipsToolCallAssistant) {
  std::string p = write_sess("st2.jsonl",
    "{\"role\":\"user\",\"content\":\"read X\"}\n"
    "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"c1\"}]}\n"
    "{\"role\":\"tool\",\"tool_call_id\":\"c1\",\"content\":\"FILE\"}\n"
    "{\"role\":\"assistant\",\"content\":\"it says FILE\"}\n");
  auto turns = extract_session_turns(p);
  ASSERT_EQ(turns.size(), 1u);                  // the user pairs with the FINAL text assistant
  EXPECT_NE(turns[0].text.find("U: read X"), std::string::npos);
  EXPECT_NE(turns[0].text.find("A: it says FILE"), std::string::npos);
}
TEST(SessionTurns, TrailingUserWithoutAssistantIsDropped) {
  std::string p = write_sess("st3.jsonl",
    "{\"role\":\"user\",\"content\":\"q1\"}\n"
    "{\"role\":\"assistant\",\"content\":\"a1\"}\n"
    "{\"role\":\"user\",\"content\":\"dangling\"}\n");
  auto turns = extract_session_turns(p);
  ASSERT_EQ(turns.size(), 1u);                  // dangling user (no answer yet) dropped
}
```

- [ ] **Step 2: Register + run, expect FAIL.** CMake:
```cmake
target_sources(hades_core PRIVATE src/embedding/session_turns.cpp)
target_sources(hades_tests PRIVATE tests/test_session_turns.cpp)
```

- [ ] **Step 3: Implement.** `include/hades/embedding/session_turns.h`:
```cpp
// include/hades/embedding/session_turns.h — extract per-turn (user+assistant) units from a session jsonl
// for embedding. Each {role:user,content:string} pairs with the NEXT {role:assistant,content:string};
// intervening tool / tool_call-assistant messages fold out (a turn is one Q + its text answer). A
// trailing user with no following assistant answer is dropped. id = "session:<basename>#<turn-index>".
#pragma once
#include <string>
#include <vector>
namespace hades {
struct SessionTurn { std::string id; std::string text; };
std::vector<SessionTurn> extract_session_turns(const std::string& session_file);
}  // namespace hades
```
`src/embedding/session_turns.cpp`:
```cpp
#include "hades/embedding/session_turns.h"
#include <filesystem>
#include "hades/session_history.h"
namespace hades {
std::vector<SessionTurn> extract_session_turns(const std::string& session_file) {
  std::vector<SessionTurn> turns;
  const auto msgs = read_session_jsonl(session_file);
  const std::string base = std::filesystem::path(session_file).filename().string();
  std::size_t idx = 0;
  for (std::size_t i = 0; i < msgs.size(); ++i) {
    if (msgs[i].value("role", "") != "user") continue;
    if (!msgs[i].contains("content") || !msgs[i]["content"].is_string()) continue;
    const std::string user = msgs[i]["content"].get<std::string>();
    // find the NEXT assistant message with string content (fold tool / tool_call assistants)
    std::string answer;
    for (std::size_t j = i + 1; j < msgs.size(); ++j) {
      if (msgs[j].value("role", "") == "user") break;             // next turn started, no answer
      if (msgs[j].value("role", "") == "assistant" && msgs[j].contains("content") && msgs[j]["content"].is_string()) {
        answer = msgs[j]["content"].get<std::string>(); break;
      }
    }
    if (answer.empty()) continue;                                  // unanswered user -> drop
    turns.push_back({"session:" + base + "#" + std::to_string(idx), "U: " + user + "\nA: " + answer});
    ++idx;
  }
  return turns;
}
}  // namespace hades
```

- [ ] **Step 4: Build + test** `-R SessionTurns` PASS, then FULL suite → **230** (228 + 2, adjust if T8's count differed; the absolute number is `previous + 3`).
- [ ] **Step 5: Commit** `feat: per-turn session extraction for embedding (user+assistant pairing, tool-fold, stable ids)`

---

## Task 10: Indexer — session corpus + live-session exclusion

**Files:** Modify `include/hades/embedding/indexer.h`, `src/embedding/indexer.cpp`. Extend `tests/test_indexer.cpp`. **Read first:** T5 (`index_archival`), T9 (`extract_session_turns`), `include/hades/session_history.h`.

**Interfaces — Produces:** `IndexStats index_sessions(EmbeddingProvider&, VectorCache&, const std::string& sessions_dir, const std::string& exclude_path, std::size_t batch_size)` — walks `sessions_dir/*.jsonl` (skipping `exclude_path`, the live session), extracts per-turn units (T9), embeds only ids not in the cache. Same batched, fail-soft contract as `index_archival`.

- [ ] **Step 1: Failing tests** — append to `tests/test_indexer.cpp`:
```cpp
#include <filesystem>
#include "hades/embedding/session_turns.h"

TEST(Indexer, IndexesSessionTurnsExcludingLive) {
  namespace fs = std::filesystem;
  std::string dir = testing::TempDir() + "/ix_sessions";
  fs::create_directories(dir);
  { std::ofstream f(dir + "/past.jsonl", std::ios::trunc);
    f << "{\"role\":\"user\",\"content\":\"q\"}\n{\"role\":\"assistant\",\"content\":\"a\"}\n"; }
  std::string live = dir + "/live.jsonl";
  { std::ofstream f(live, std::ios::trunc);
    f << "{\"role\":\"user\",\"content\":\"now\"}\n{\"role\":\"assistant\",\"content\":\"here\"}\n"; }
  std::string cache = testing::TempDir() + "/ix_sess_cache.jsonl";
  std::remove(cache.c_str());
  FakeProvider prov;                            // reuse the FakeProvider from the archival tests
  VectorCache vc(cache, "fake", 2); ASSERT_TRUE(vc.load());
  auto st = index_sessions(prov, vc, dir, live, 32);
  EXPECT_TRUE(st.ok);
  EXPECT_EQ(st.embedded, 1u);                   // only past.jsonl's single turn; live excluded
  EXPECT_EQ(vc.size(), 1u);
}
```
(The `FakeProvider` defined earlier in `test_indexer.cpp` is reused; ensure it is declared before this test or in the file's anonymous namespace.)

- [ ] **Step 2: Run, expect FAIL** (no `index_sessions`).

- [ ] **Step 3: Implement.** Add to `include/hades/embedding/indexer.h`:
```cpp
IndexStats index_sessions(EmbeddingProvider& provider, VectorCache& cache,
                          const std::string& sessions_dir, const std::string& exclude_path,
                          std::size_t batch_size);
```
In `src/embedding/indexer.cpp` add (reuse the same `flush_batch` helper, but with `src="session"`; generalize `flush_batch` to take a `src` argument, or add a sibling):
```cpp
#include <filesystem>
#include "hades/embedding/session_turns.h"
// ... extend the anonymous-namespace flush_batch to accept a src label:
//   bool flush_batch(provider, cache, batch, st, const std::string& src)
//   -> cache.put({id, src, text, vec}); update the index_archival call to pass "memory".

IndexStats index_sessions(EmbeddingProvider& provider, VectorCache& cache,
                          const std::string& sessions_dir, const std::string& exclude_path,
                          std::size_t batch_size) {
  namespace fs = std::filesystem;
  IndexStats st;
  if (batch_size == 0) batch_size = 1;
  std::error_code ec;
  if (!fs::exists(sessions_dir, ec)) return st;
  std::vector<std::pair<std::string, std::string>> batch;
  fs::path excl = fs::weakly_canonical(fs::path(exclude_path), ec);
  for (const auto& entry : fs::directory_iterator(sessions_dir, ec)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".jsonl") continue;
    if (fs::weakly_canonical(entry.path(), ec) == excl) continue;     // skip the live session
    for (const auto& t : extract_session_turns(entry.path().string())) {
      if (cache.has(t.id)) { ++st.skipped; continue; }
      batch.emplace_back(t.id, t.text);
      if (batch.size() >= batch_size && !flush_batch(provider, cache, batch, st, "session")) return st;
    }
  }
  flush_batch(provider, cache, batch, st, "session");
  return st;
}
```
Then in `EmbeddingMemoryModule::run_index_` (T6), after `index_archival`, also call `index_sessions` when `index_sessions_` config is true, passing the live session path. **This requires the module to know two things it doesn't have yet:** the `sessions_dir` and the live session path. Add config members `sessions_dir_` (default `.hades/sessions`), `index_sessions_` (default true), and a `std::string live_session_path_` with a setter `set_live_session_path(std::string)` that **wire_agent sets to the same `session_path` the Arbiter uses** (so the live file is excluded). Read `sessions_dir`/`index_sessions` in `on_start`. Guard: if `index_sessions_` is false, skip the session pass.

- [ ] **Step 4: Build + test** `-R Indexer` PASS, then FULL suite (previous + 1). Also extend the wiring (T8 site): `if (a.embedding) a.embedding->set_live_session_path(<session_path>);` — the implementer adds this to `wire_agent` where the session path is available (it is the same value passed to `arbiter->set_session_path`; if `wire_agent` doesn't have it, thread it in or set it in `hades_main` right after `arbiter->set_session_path`). Add a test in `test_embedding_memory_module.cpp` that with `index_sessions=true` + a temp sessions dir, a past session's turn is retrievable.
- [ ] **Step 5: Commit** `feat: index past-session corpus (per-turn, live-session excluded) into the embedding cache`

---

## Task 11: Periodic reindex timer

**Files:** Modify `include/hades/module/embedding_memory_module.h`, `src/module/embedding_memory_module.cpp`. Extend `tests/test_embedding_memory_module.cpp`. **Read first:** T6, `include/hades/executor.h`.

**Interfaces — Produces:** `reindex_interval_s` config (default `kDefaultReindexIntervalS=86400`; `0`=off); a dedicated timer thread that re-runs `run_index_` every interval until stop, joined at teardown.

- [ ] **Step 1: Failing test** — append to `tests/test_embedding_memory_module.cpp` (test the **config parse + that a tiny interval triggers at least one extra index**, using a short interval and a CountingProvider; keep it fast and deterministic by asserting the reindex *ran*, not exact timing):
```cpp
TEST(EmbeddingMemoryModule, ReindexIntervalParsedAndDefaulted) {
  // default (absent) -> 86400; explicit 0 -> off; explicit 5 -> 5. Expose via a getter for the test.
  Blackboard bb;
  EmbeddingMemoryModule m(std::make_unique<FakeProvider>());
  Block b = cfg(/*store*/ testing::TempDir() + "/ri_store.jsonl", testing::TempDir() + "/ri_cache");
  { std::ofstream f(b.kv["memory_store"], std::ios::trunc); f << "{\"text\":\"x\",\"ts\":1}\n"; }
  b.kv["reindex_interval_s"] = "0";
  m.on_start(b, bb);
  EXPECT_DOUBLE_EQ(m.reindex_interval_s(), 0.0);   // add the getter
}
```
(Testing the live timer firing is timing-dependent; keep the unit test to the config seam + the index function, and verify the timer thread manually. State this in the report.)

- [ ] **Step 2: Run, expect FAIL** (no `reindex_interval_s()` getter / no parse).

- [ ] **Step 3: Implement.**
  - Header: add members `double reindex_interval_s_ = kDefaultReindexIntervalS;`, `std::thread reindex_thread_;`, `std::atomic<bool> stop_reindex_{false};`, a `std::condition_variable`/`std::mutex` for an interruptible wait, and a getter `double reindex_interval_s() const { return reindex_interval_s_; }`. Add `#include <thread>`, `#include <atomic>`, `#include <condition_variable>`, `#include <mutex>`. Add a destructor `~EmbeddingMemoryModule();` (join the timer).
  - `on_start`: parse `reindex_interval_s` via `set_pos_double_on_string` BUT allow `0` (off) — `0` is valid here, so parse with a plain double parse that accepts `>=0`: if the key is `"0"` set 0; else `set_pos_double_on_string`. Document `0`=off.
  - `on_attach`: after kicking the initial index, if `reindex_interval_s_ > 0` start the timer thread:
    ```cpp
    if (reindex_interval_s_ > 0) {
      reindex_thread_ = std::thread([this] {
        std::unique_lock<std::mutex> lk(reindex_mu_);
        while (!stop_reindex_) {
          if (reindex_cv_.wait_for(lk, std::chrono::duration<double>(reindex_interval_s_),
                                   [this] { return stop_reindex_.load(); })) break;  // stopped
          lk.unlock();
          run_index_();                          // incremental: embeds only NEW records
          lk.lock();
        }
      });
    }
    ```
    NOTE: `run_index_` posts to the bus (`EMBED_INDEX_DONE`) — `post()` is thread-safe, fine. But `run_index_` must NOT touch any module state the pump thread also mutates; it only reads config + writes the cache file (the query path re-opens the cache per turn, so a concurrent index-append is tolerated like any concurrent cache writer — document this; the cache file append is the shared resource, and `VectorCache::put` appends a line, while the query opens a fresh read — a half-written final line is skipped by the tolerant load). To avoid an index-thread vs initial-index race on the SAME cache file, the timer only runs AFTER on_attach (the initial index, if inline, has completed; if on an Executor, both are incremental + the file append is append-only — acceptable, documented).
  - `~EmbeddingMemoryModule()`: `{ std::lock_guard<std::mutex> lk(reindex_mu_); stop_reindex_ = true; } reindex_cv_.notify_all(); if (reindex_thread_.joinable()) reindex_thread_.join();`
  - Teardown ordering: the module's dtor joins its timer thread; the module is owned by `Agent` and destroyed before the Blackboard (the existing teardown contract), so `bb_->post` from a late timer tick can't outlive the bus — BUT to be safe, the dtor sets stop + joins FIRST, so no tick runs during/after Blackboard destruction. Confirm `Agent.embedding` is declared among the modules (T8) so it is destroyed before `bb` in `hades_main`.

- [ ] **Step 4: Build + test** `-R EmbeddingMemoryModule` PASS, then FULL suite green. Run under **TSan** for this module's tests if the suite has a TSan config (`the index thread + query path share the cache file`): `nix develop --command ctest --test-dir build -R EmbeddingMemoryModule` after a TSan build, and report the result (the project keeps TSan clean).
- [ ] **Step 5: Commit** `feat: periodic reindex timer (reindex_interval_s, default daily; 0=off) with clean teardown`

---

## Self-Review (against the spec)

- **Coverage:** provider iface + vec_math (T1); HttpEmbeddingProvider (T2); warm SubprocessEmbeddingProvider + PersistentChild + mutex (T3); model-stamped cache + cosine + floor + rebuild-on-mismatch (T4); incremental archival indexer (T5); EmbeddingMemoryModule query path + provider factory + inline/executor index + fail-soft (T6); Arbiter hybrid dedup-merge (T7); wiring + manifest + reference embedder + docs (T8); session per-turn extraction (T9); session-corpus index + live exclusion (T10); periodic daily reindex (T11). Defaults single-sourced (T1). Reuses `load_memories` + `read_session_jsonl` (T5/T9).
- **THE rule** (same model+dim docs+query) enforced: cache stamp + `load()→false→rebuild` (T4), probe-then-stamp (T6), query asserts model via re-open (T6). **Fail-soft** at every external boundary (T2/T3/T6). **Concurrency:** provider mutex (T3) + per-turn cache re-open (T6) + timer teardown (T11).
- **Backward-compat:** embedding module inert unless rostered; test `build_agent` overload never builds it; Arbiter merge identical when semantic absent (T7 second test). Existing 204 stay green; counts climb 208→…→~231.
- **Type consistency:** `EmbedResult{vectors,model,dim,error}`, `EmbeddingProvider::embed/model`, `VectorCache(path,model,dim)::{load,has,ids,put,query,clear_file,size}`, `CachedVec{id,src,text,vec}`, `ScoredMemory{text,score}`, `IndexStats{embedded,skipped,ok}`, `index_archival/index_sessions`, `SessionTurn{id,text}`/`extract_session_turns`, `RETRIEVED_MEMORY_SEMANTIC`, `set_executor`/`set_live_session_path` — used identically across tasks.

## Verification

1. Full suite green at every task (no regressions; ~231 at the end).
2. Provider fail-soft: missing binary / 500 / malformed json / count mismatch → `error` set, never a throw (T2/T3).
3. Cache: round-trip, model-mismatch→rebuild, degenerate dropped, cosine+floor ordering (T4).
4. Indexer incremental skip; session per-turn + live exclusion (T5/T9/T10).
5. Hybrid merge dedups; semantic-absent identical to today (T7).
6. Wiring: rostered → module built + wired before Arbiter + executor set; absent → null; dev.hades still builds (T8).
7. **Live smoke (Vaios, reference embedder):** `pip install sentence-transformers`; uncomment the `dev.hades` Embedding block (`command = python3 ./tools/embed_reference.py`); chat to save a memory or two; restart; ask a **paraphrased** question and confirm a semantically-relevant memory is recalled that keyword alone would miss; stop the embedder mid-run and confirm the agent still answers (fail-soft). TSan-clean for the module (T11).
