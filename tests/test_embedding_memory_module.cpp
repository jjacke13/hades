#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include "hades/module/embedding_memory_module.h"
#include "hades/blackboard.h"
#include "hades/config.h"
#include "hades/embedding/defaults.h"
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
struct CountingProvider : EmbeddingProvider {    // atomic call counter so a timer tick is observable
  std::atomic<int> calls{0};
  EmbedResult embed(const std::vector<std::string>& in) override {
    calls.fetch_add(1, std::memory_order_relaxed);
    EmbedResult r; r.model = "fake"; r.dim = 2;
    for (std::size_t i = 0; i < in.size(); ++i) r.vectors.push_back({1.0f, 0.0f});
    return r;
  }
  std::string model() const override { return "fake"; }
};
struct ShiftingProvider : EmbeddingProvider {   // index/probe -> "modelA"; a "SHIFT" query -> "modelB"
  EmbedResult embed(const std::vector<std::string>& in) override {
    bool shift = false;
    for (const auto& t : in) if (t.find("SHIFT") != std::string::npos) shift = true;
    EmbedResult r; r.dim = 2; r.model = shift ? "modelB" : "modelA";
    for (std::size_t i = 0; i < in.size(); ++i) r.vectors.push_back({1.0f, 0.0f});
    return r;
  }
  std::string model() const override { return "modelA"; }
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
  std::string sess;
  bb.subscribe("RETRIEVED_MEMORY_SEMANTIC", [&](const Entry& e) { got = e.value.get<std::string>(); });
  bb.subscribe("RETRIEVED_SESSION_SEMANTIC", [&](const Entry& e) { sess = e.value.get<std::string>(); });
  bb.post("USER_MESSAGE", "tell me about alpha", "chat");
  bb.pump();
  EXPECT_NE(got.find("alpha fact"), std::string::npos);
  EXPECT_EQ(got.find("beta fact"), std::string::npos);  // below floor for an alpha query
  EXPECT_EQ(sess, "");                                   // archival-only store -> no session excerpts
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
  std::string sess = "UNSET";
  bb.subscribe("RETRIEVED_MEMORY_SEMANTIC", [&](const Entry& e) { got = e.value.get<std::string>(); });
  bb.subscribe("RETRIEVED_SESSION_SEMANTIC", [&](const Entry& e) { sess = e.value.get<std::string>(); });
  bb.post("USER_MESSAGE", "alpha?", "chat");
  bb.pump();                                    // must NOT crash
  EXPECT_EQ(got, "");                            // fail-soft: empty
  EXPECT_EQ(sess, "");                           // fail-soft: both keys empty
}
TEST(EmbeddingMemoryModule, CacheStampMismatchAtQueryIsSoftEmpty) {
  // Defense-in-depth (path C): if the query's embedding model differs from the cache's stamp, the
  // query must fail-soft (never compare incomparable vectors) -> "" / keyword-only. run_index_
  // stamps the cache "modelA"; the "SHIFT" query returns "modelB" -> VectorCache.load() mismatch.
  std::string store = tmp("em_store3.jsonl"), cache = tmp("em_cache3");
  { std::ofstream f(store, std::ios::trunc); f << "{\"text\":\"alpha fact\",\"ts\":1}\n"; }
  std::remove((cache + "/memory.vec.jsonl").c_str());
  Blackboard bb;
  EmbeddingMemoryModule m(std::make_unique<ShiftingProvider>());
  m.on_start(cfg(store, cache), bb);
  m.on_attach(bb);
  std::string got = "UNSET";
  std::string sess = "UNSET";
  bb.subscribe("RETRIEVED_MEMORY_SEMANTIC", [&](const Entry& e) { got = e.value.get<std::string>(); });
  bb.subscribe("RETRIEVED_SESSION_SEMANTIC", [&](const Entry& e) { sess = e.value.get<std::string>(); });
  bb.post("USER_MESSAGE", "SHIFT please", "chat");
  bb.pump();
  EXPECT_EQ(got, "");                            // stamp mismatch -> fail-soft empty
  EXPECT_EQ(sess, "");                           // stamp mismatch -> both keys empty
}
TEST(EmbeddingMemoryModule, ReindexIntervalParsedAndDefaulted) {
  // The config seam for the periodic reindex timer: absent -> daily default; "0" -> off; "5" -> 5s.
  // (Testing the live timer FIRING is timing-dependent; covered by inspection/manual — see report.)
  std::string store = tmp("em_ri_store.jsonl"), cache = tmp("em_ri_cache");
  { std::ofstream f(store, std::ios::trunc); f << "{\"text\":\"alpha\",\"ts\":1}\n"; }
  Blackboard bb;
  {  // absent key -> default 86400
    EmbeddingMemoryModule m(std::make_unique<FakeProvider>());
    m.on_start(cfg(store, cache), bb);
    EXPECT_DOUBLE_EQ(m.reindex_interval_s(), kDefaultReindexIntervalS);
  }
  {  // explicit "0" -> off (0 is valid here, unlike set_pos_double_on_string)
    EmbeddingMemoryModule m(std::make_unique<FakeProvider>());
    Block b = cfg(store, cache); b.kv["reindex_interval_s"] = "0";
    m.on_start(b, bb);
    EXPECT_DOUBLE_EQ(m.reindex_interval_s(), 0.0);
  }
  {  // explicit "5" -> 5
    EmbeddingMemoryModule m(std::make_unique<FakeProvider>());
    Block b = cfg(store, cache); b.kv["reindex_interval_s"] = "5";
    m.on_start(b, bb);
    EXPECT_DOUBLE_EQ(m.reindex_interval_s(), 5.0);
  }
  {  // "0.0" (not just literal "0") also -> off; a negative -> keep default (never silent-disable)
    EmbeddingMemoryModule m(std::make_unique<FakeProvider>());
    Block b = cfg(store, cache); b.kv["reindex_interval_s"] = "0.0";
    m.on_start(b, bb);
    EXPECT_DOUBLE_EQ(m.reindex_interval_s(), 0.0);
    EmbeddingMemoryModule m2(std::make_unique<FakeProvider>());
    Block b2 = cfg(store, cache); b2.kv["reindex_interval_s"] = "-3";
    m2.on_start(b2, bb);
    EXPECT_DOUBLE_EQ(m2.reindex_interval_s(), kDefaultReindexIntervalS);
  }
  {  // garbage -> keep default
    EmbeddingMemoryModule m(std::make_unique<FakeProvider>());
    Block b = cfg(store, cache); b.kv["reindex_interval_s"] = "not-a-number";
    m.on_start(b, bb);
    EXPECT_DOUBLE_EQ(m.reindex_interval_s(), kDefaultReindexIntervalS);
  }
}
TEST(EmbeddingMemoryModule, TimerStartsAndJoinsCleanly) {
  // A positive interval starts a background timer thread. With a long interval (3600s) it never fires
  // during the test, but the module's destructor MUST stop+notify+join it promptly (no hang, no crash,
  // no use-after-free against the Blackboard). This guards the teardown invariant.
  std::string store = tmp("em_timer_store.jsonl"), cache = tmp("em_timer_cache");
  { std::ofstream f(store, std::ios::trunc); f << "{\"text\":\"alpha\",\"ts\":1}\n"; }
  std::remove((cache + "/memory.vec.jsonl").c_str());
  Blackboard bb;
  {
    EmbeddingMemoryModule m(std::make_unique<FakeProvider>());
    Block b = cfg(store, cache); b.kv["reindex_interval_s"] = "3600";
    m.on_start(b, bb);
    EXPECT_DOUBLE_EQ(m.reindex_interval_s(), 3600.0);
    m.on_attach(bb);                               // starts the timer thread (inline, no executor)
    bb.post("USER_MESSAGE", "tell me about alpha", "chat");
    bb.pump();                                     // normal turn while the timer is alive
  }  // m destructs here -> dtor stops+joins the timer; must NOT hang
  SUCCEED();
}
TEST(EmbeddingMemoryModule, TimerFiresPeriodically) {
  // A tiny interval makes the timer re-run run_index_ — exercising the wait_for-timeout -> unlock/run/
  // relock loop body (otherwise only covered by a manual harness). Bounded poll (no fixed sleep, no
  // flake): wait up to ~3s for the provider's embed-call count to rise past a post-attach snapshot.
  std::string store = tmp("em_fire_store.jsonl"), cache = tmp("em_fire_cache");
  { std::ofstream f(store, std::ios::trunc); f << "{\"text\":\"alpha\",\"ts\":1}\n"; }
  std::remove((cache + "/memory.vec.jsonl").c_str());
  Blackboard bb;
  auto up = std::make_unique<CountingProvider>();
  CountingProvider* cp = up.get();               // raw view for polling; module owns `up`
  EmbeddingMemoryModule m(std::move(up));
  Block b = cfg(store, cache); b.kv["reindex_interval_s"] = "0.05";   // fires fast
  m.on_start(b, bb);
  m.on_attach(bb);                                 // initial inline index + timer starts
  const int after_attach = cp->calls.load();       // calls so far (initial index)
  bool fired = false;
  for (int i = 0; i < 300 && !fired; ++i) {        // up to ~3s
    if (cp->calls.load() > after_attach) fired = true;
    else std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(fired);                              // a periodic tick re-ran run_index_
  // m destructs at scope exit -> stops+joins the fast timer; must not hang.
}
TEST(EmbeddingMemoryModule, RetrievesPastSessionTurnWhenIndexSessionsEnabled) {
  // With index_sessions=true + a sessions dir holding ONE past session, the per-turn unit of that
  // session is embedded into the same cache and is retrievable on a matching query (no live path
  // set -> nothing excluded). The archival store is empty -> the hit comes from the session corpus.
  namespace fs = std::filesystem;
  std::string store = tmp("em_sess_store.jsonl"), cache = tmp("em_sess_cache");
  { std::ofstream f(store, std::ios::trunc); }   // empty archival store
  std::string sdir = tmp("em_sessions");
  fs::remove_all(sdir);
  fs::create_directories(sdir);
  { std::ofstream f(sdir + "/past.jsonl", std::ios::trunc);
    f << "{\"role\":\"user\",\"content\":\"what is alpha\"}\n"
         "{\"role\":\"assistant\",\"content\":\"alpha is the first letter\"}\n"; }
  std::remove((cache + "/memory.vec.jsonl").c_str());
  Block b; b.section = "Embedding";
  b.kv["memory_store"] = store; b.kv["cache_dir"] = cache; b.kv["min_similarity"] = "0.2";
  b.kv["index_sessions"] = "true"; b.kv["sessions_dir"] = sdir;
  Blackboard bb;
  EmbeddingMemoryModule m(std::make_unique<FakeProvider>());
  m.on_start(b, bb);                             // inline index (no executor) of archival + sessions
  m.on_attach(bb);
  std::string got;
  std::string facts = "UNSET";
  bb.subscribe("RETRIEVED_SESSION_SEMANTIC", [&](const Entry& e) { got = e.value.get<std::string>(); });
  bb.subscribe("RETRIEVED_MEMORY_SEMANTIC", [&](const Entry& e) { facts = e.value.get<std::string>(); });
  bb.post("USER_MESSAGE", "tell me about alpha", "chat");
  bb.pump();
  EXPECT_NE(got.find("alpha is the first letter"), std::string::npos);  // the past session's turn -> SESSION key
  EXPECT_EQ(facts, "");                                                 // session-only store -> fact key empty (no leak)
}
TEST(EmbeddingMemoryModule, RetrievesMixedFactAndSessionSplitsAcrossBothKeys) {
  // The behavior the split exists for: one query returning BOTH a fact hit and a session hit, each
  // landing on its own non-empty key. Archival store has an "alpha fact"; the sessions dir has an
  // "alpha" turn; FakeProvider maps "alpha" -> (1,0) so both records + the query align (cosine 1).
  namespace fs = std::filesystem;
  std::string store = tmp("em_mix_store.jsonl"), cache = tmp("em_mix_cache");
  { std::ofstream f(store, std::ios::trunc); f << "{\"text\":\"alpha fact\",\"ts\":1}\n"; }
  std::string sdir = tmp("em_mix_sessions");
  fs::remove_all(sdir);
  fs::create_directories(sdir);
  { std::ofstream f(sdir + "/past.jsonl", std::ios::trunc);
    f << "{\"role\":\"user\",\"content\":\"alpha please\"}\n"
         "{\"role\":\"assistant\",\"content\":\"alpha is first\"}\n"; }
  std::remove((cache + "/memory.vec.jsonl").c_str());
  Block b; b.section = "Embedding";
  b.kv["memory_store"] = store; b.kv["cache_dir"] = cache; b.kv["min_similarity"] = "0.2";
  b.kv["index_sessions"] = "true"; b.kv["sessions_dir"] = sdir;
  Blackboard bb;
  EmbeddingMemoryModule m(std::make_unique<FakeProvider>());
  m.on_start(b, bb);
  m.on_attach(bb);
  std::string facts, sess;
  bb.subscribe("RETRIEVED_MEMORY_SEMANTIC", [&](const Entry& e) { facts = e.value.get<std::string>(); });
  bb.subscribe("RETRIEVED_SESSION_SEMANTIC", [&](const Entry& e) { sess = e.value.get<std::string>(); });
  bb.post("USER_MESSAGE", "tell me about alpha", "chat");
  bb.pump();
  EXPECT_NE(facts.find("alpha fact"), std::string::npos);   // archival hit -> FACT key
  EXPECT_NE(sess.find("alpha is first"), std::string::npos); // session hit -> SESSION key
}

// ── SESSION_ROTATED: the live-session exclusion follows the rotation (daily sessions) ────────────
//
// A session is a DAY, so the live file moves under a running process. Before this, the exclusion
// was written once at wiring time and then went stale at the first rollover: the CLOSED start-day
// file stayed skipped while today's still-being-appended one was indexed and came back injected as
// "excerpts from earlier sessions". These tests drive the real thing — a fast periodic reindex, an
// EMBED_INDEX_DONE count as the completion signal, and orthogonal FakeProvider words so which of
// two session files is in the index is directly observable from a query.
namespace {
// One session file holding a single turn built around `word` ("alpha" -> (1,0), "beta" -> (0,1)
// in FakeProvider, so a query tells two files apart).
void write_turn(const std::string& dir, const std::string& file, const std::string& word) {
  std::ofstream o(dir + "/" + file, std::ios::trunc);
  o << "{\"role\":\"user\",\"content\":\"about " << word << "\"}\n"
    << "{\"role\":\"assistant\",\"content\":\"" << word << " session turn\"}\n";
}
std::string empty_sessions_dir(const char* tag) {
  namespace fs = std::filesystem;
  const std::string d = tmp(std::string("em_rot_") + tag);
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}
Block session_cfg(const std::string& store, const std::string& cache, const std::string& sdir) {
  Block b; b.section = "Embedding";
  b.kv["memory_store"] = store; b.kv["cache_dir"] = cache; b.kv["min_similarity"] = "0.2";
  b.kv["index_sessions"] = "true"; b.kv["sessions_dir"] = sdir;
  b.kv["reindex_interval_s"] = "0.05";     // the rotation is picked up by the next periodic run
  return b;
}
// Drive one USER_MESSAGE turn and read back RETRIEVED_SESSION_SEMANTIC. Subscribes ONCE, in the
// constructor: a subscribe-per-call would leave the Blackboard holding lambdas that capture a
// local which is already gone.
struct Recall {
  explicit Recall(Blackboard& bb) : bb_(bb) {
    bb.subscribe("RETRIEVED_SESSION_SEMANTIC",
                 [this](const Entry& e) { got_ = e.value.get<std::string>(); });
  }
  std::string operator()(const std::string& query) {
    got_.clear();
    bb_.post("USER_MESSAGE", query, "chat");
    bb_.pump();
    return got_;
  }
private:
  Blackboard& bb_;
  std::string got_;
};
// Wait for the periodic reindex to COMPLETE one more run: EMBED_INDEX_DONE is posted at the end of
// run_index_ (from the timer thread; post() is thread-safe) and counted here on the pump thread, so
// this is a real completion signal rather than a sleep.
bool wait_for_reindex(Blackboard& bb, const int& runs) {
  const int before = runs;
  for (int i = 0; i < 400; ++i) {                        // up to ~4s
    bb.pump();
    if (runs > before) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}
}  // namespace

TEST(EmbeddingMemoryModule, SessionRotatedRepointsTheLiveExclusion) {
  std::string store = tmp("em_rot_store.jsonl"), cache = tmp("em_rot_cache");
  { std::ofstream f(store, std::ios::trunc); }                       // empty archival store
  std::remove((cache + "/memory.vec.jsonl").c_str());
  const std::string sdir = empty_sessions_dir("day");
  write_turn(sdir, "2026-09-06.jsonl", "alpha");                     // the day we boot into
  Blackboard bb;
  int runs = 0;
  bb.subscribe("EMBED_INDEX_DONE", [&](const Entry&) { ++runs; });
  Recall recall(bb);
  EmbeddingMemoryModule m(std::make_unique<FakeProvider>());
  m.on_start(session_cfg(store, cache, sdir), bb);
  m.set_live_session_path(sdir + "/2026-09-06.jsonl");               // yesterday is live at boot
  m.on_attach(bb);                                                   // initial inline index
  bb.pump();
  EXPECT_EQ(recall("about alpha").find("alpha session turn"), std::string::npos)
      << "the live session must not be indexed";
  // Midnight passes: the Arbiter rotates onto the new day's (brand-new) file and announces it.
  write_turn(sdir, "2026-09-07.jsonl", "beta");
  bb.post("SESSION_ROTATED", {{"from", "2026-09-06"}, {"to", "2026-09-07"},
                              {"path", sdir + "/2026-09-07.jsonl"}}, "arbiter");
  bb.pump();
  ASSERT_TRUE(wait_for_reindex(bb, runs));
  // Yesterday's file is closed now, so it belongs in the corpus...
  EXPECT_NE(recall("about alpha").find("alpha session turn"), std::string::npos);
  // ...and the file the process is now appending to is the one being skipped.
  EXPECT_EQ(recall("about beta").find("beta session turn"), std::string::npos);
}

// The retired gotcha: `/new` rotates to a same-day SIBLING (2026-09-07 -> 2026-09-07-1) and posts
// the same event, so the exclusion follows there too. Before this it did not, and CLAUDE.md carried
// that as a documented accepted bug.
TEST(EmbeddingMemoryModule, RotationToASuffixedNewSessionAlsoRepoints) {
  std::string store = tmp("em_new_store.jsonl"), cache = tmp("em_new_cache");
  { std::ofstream f(store, std::ios::trunc); }
  std::remove((cache + "/memory.vec.jsonl").c_str());
  const std::string sdir = empty_sessions_dir("new");
  write_turn(sdir, "2026-09-07.jsonl", "alpha");
  Blackboard bb;
  int runs = 0;
  bb.subscribe("EMBED_INDEX_DONE", [&](const Entry&) { ++runs; });
  Recall recall(bb);
  EmbeddingMemoryModule m(std::make_unique<FakeProvider>());
  m.on_start(session_cfg(store, cache, sdir), bb);
  m.set_live_session_path(sdir + "/2026-09-07.jsonl");
  m.on_attach(bb);
  bb.pump();
  write_turn(sdir, "2026-09-07-1.jsonl", "beta");                    // `/new`'s same-day sibling
  bb.post("SESSION_ROTATED", {{"from", "2026-09-07"}, {"to", "2026-09-07-1"},
                              {"path", sdir + "/2026-09-07-1.jsonl"}}, "chat");
  bb.pump();
  ASSERT_TRUE(wait_for_reindex(bb, runs));
  EXPECT_NE(recall("about alpha").find("alpha session turn"), std::string::npos);
  EXPECT_EQ(recall("about beta").find("beta session turn"), std::string::npos);
}

// An Arbiter with no sessions_dir rotates to nowhere and posts {to:"",path:""}. Adopting that
// blindly would leave NO live session excluded — i.e. index the file being appended to right now —
// so an empty path is ignored and the last known-good exclusion stands.
TEST(EmbeddingMemoryModule, EmptyRotationPathIsIgnored) {
  std::string store = tmp("em_empty_store.jsonl"), cache = tmp("em_empty_cache");
  { std::ofstream f(store, std::ios::trunc); }
  std::remove((cache + "/memory.vec.jsonl").c_str());
  const std::string sdir = empty_sessions_dir("empty");
  write_turn(sdir, "past.jsonl", "alpha");
  write_turn(sdir, "live.jsonl", "beta");
  Blackboard bb;
  int runs = 0;
  bb.subscribe("EMBED_INDEX_DONE", [&](const Entry&) { ++runs; });
  Recall recall(bb);
  EmbeddingMemoryModule m(std::make_unique<FakeProvider>());
  m.on_start(session_cfg(store, cache, sdir), bb);
  m.set_live_session_path(sdir + "/live.jsonl");
  m.on_attach(bb);
  bb.pump();
  bb.post("SESSION_ROTATED", {{"from", "live"}, {"to", ""}, {"path", ""}}, "arbiter");
  bb.pump();
  ASSERT_TRUE(wait_for_reindex(bb, runs));                     // a full run happened...
  EXPECT_NE(recall("about alpha").find("alpha session turn"), std::string::npos);
  EXPECT_EQ(recall("about beta").find("beta session turn"), std::string::npos)
      << "an empty rotation path must not clear the exclusion";
}
