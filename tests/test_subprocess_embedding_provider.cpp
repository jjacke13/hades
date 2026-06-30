#include <gtest/gtest.h>
#include "hades/embedding/subprocess_embedding_provider.h"
using namespace hades;

TEST(SubprocessEmbeddingProvider, EmbedsBatchOverWarmChild) {
  SubprocessEmbeddingProvider p({"node", ECHO_EMBEDDER}, 10.0);
  auto r = p.embed({"hello", "world"});
  EXPECT_TRUE(r.error.empty());
  ASSERT_EQ(r.vectors.size(), 2u);
  EXPECT_EQ(r.dim, 3);
  EXPECT_EQ(r.model, "echo");
}
TEST(SubprocessEmbeddingProvider, SecondCallReusesSameWarmChild) {
  SubprocessEmbeddingProvider p({"node", ECHO_EMBEDDER}, 10.0);
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
TEST(SubprocessEmbeddingProvider, MalformedReplyIsSoftError) {
  // A present-but-wrong-typed "error" field ({"error":{...}}) used to make j.value("error", ...)
  // throw type_error.302, escaping embed(). It must now be a soft error — no throw, no crash.
  SubprocessEmbeddingProvider p({"node", ECHO_EMBEDDER}, 10.0);
  EmbedResult r;
  ASSERT_NO_THROW({ r = p.embed({"__BADREPLY__"}); });
  EXPECT_FALSE(r.error.empty());
  EXPECT_TRUE(r.vectors.empty());
}
TEST(SubprocessEmbeddingProvider, RespawnsAfterChildDeath) {
  // The warm child exits with no reply -> EOF soft error; the next call must respawn a fresh child.
  SubprocessEmbeddingProvider p({"node", ECHO_EMBEDDER}, 10.0);
  auto r1 = p.embed({"__DIE__"});
  EXPECT_FALSE(r1.error.empty());              // EOF -> soft error
  auto r2 = p.embed({"hello"});                // respawned -> succeeds
  EXPECT_TRUE(r2.error.empty());
  ASSERT_EQ(r2.vectors.size(), 1u);
}
