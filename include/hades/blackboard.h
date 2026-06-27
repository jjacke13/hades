// include/hades/blackboard.h — central pub/sub store; sole inter-module bus
//
// Maintains a latest-value key->Entry map and a FIFO event queue drained by
// pump(). Modules subscribe with exact, "PREFIX*", or "*" patterns and receive
// Handler callbacks on pump(); they post via post() — never calling each other
// directly. Optionally forwards every Entry to an Eventlog for transcript and
// replay.

#pragma once
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "hades/entry.h"
namespace hades {
class Eventlog;
class Blackboard {
public:
  explicit Blackboard(Eventlog* log = nullptr);
  ~Blackboard();
  void subscribe(const std::string& pattern, Handler h, double min_interval = 0.0); // exact|"PFX*"|"*"
  void post(const std::string& key, nlohmann::json value,
            const std::string& source, const std::string& aux = "");
  std::optional<Entry> get(const std::string& key) const;
  void pump();                 // drain queue until empty (handlers may post more)
  std::size_t queued() const;
  double now() const;          // seconds since construction
private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};
}  // namespace hades
