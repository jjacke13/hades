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
