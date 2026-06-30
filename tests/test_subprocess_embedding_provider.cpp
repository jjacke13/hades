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
