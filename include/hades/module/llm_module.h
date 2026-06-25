#pragma once
#include <memory>
#include "hades/module.h"
#include "hades/llm/provider.h"
namespace hades {
class Blackboard;
class LLMModule : public Module {
public:
  LLMModule() = default;
  explicit LLMModule(std::unique_ptr<Provider> p);   // test injection
  std::string type() const override { return "llm"; }
  void on_start(const Block& cfg, Blackboard& bb) override;
  void on_attach(Blackboard& bb) override;
private:
  std::unique_ptr<Provider> provider_;
  double price_per_mtok_ = 0.0, spent_ = 0.0;
  Blackboard* bb_ = nullptr;
};
}  // namespace hades
