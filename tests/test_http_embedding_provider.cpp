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
TEST(HttpEmbeddingProvider, DimInconsistentIsSoftError) {
  // Two vectors of different length (dim 2 then dim 1) -> the post-loop dim guard must soft-error.
  HttpEmbeddingProvider p("https://x/v1", "k", "m",
    [](auto, auto, auto) { return HttpResponse{200, R"({"data":[{"embedding":[1.0,0.0]},{"embedding":[1.0]}]})"}; });
  auto r = p.embed({"a", "b"});
  EXPECT_FALSE(r.error.empty());
  EXPECT_TRUE(r.vectors.empty());
}
TEST(HttpEmbeddingProvider, MalformedItemIsSoftError) {
  // A data item missing/!array "embedding", or a non-number value, must set error (fail-soft) —
  // regression: an earlier `return {}` discarded the just-set error, looking like empty success.
  HttpEmbeddingProvider p1("https://x/v1", "k", "m",
    [](auto, auto, auto) { return HttpResponse{200, R"({"data":[{"embedding":"oops"}]})"}; });
  auto r1 = p1.embed({"a"});
  EXPECT_FALSE(r1.error.empty());
  EXPECT_TRUE(r1.vectors.empty());
  HttpEmbeddingProvider p2("https://x/v1", "k", "m",
    [](auto, auto, auto) { return HttpResponse{200, R"({"data":[{"embedding":["x"]}]})"}; });
  auto r2 = p2.embed({"a"});
  EXPECT_FALSE(r2.error.empty());
  EXPECT_TRUE(r2.vectors.empty());
}
