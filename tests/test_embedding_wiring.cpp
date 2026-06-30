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
