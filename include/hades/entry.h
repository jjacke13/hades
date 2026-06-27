// include/hades/entry.h — the atomic message type on the Blackboard
//
// Defines Entry: every post() on the Blackboard produces one Entry (key, JSON
// value, source module, aux provenance, timestamp, monotonic seq). The Eventlog
// appends each Entry to disk; Handlers receive a const Entry& on delivery.

#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <nlohmann/json.hpp>
namespace hades {
struct Entry {
  std::string    key;
  nlohmann::json value;   // single JSON payload type
  std::string    source;  // module that posted it
  std::string    aux;     // provenance; "" if none
  double         ts;      // seconds since session start
  std::uint64_t  seq;     // monotonic post counter
};
using Handler = std::function<void(const Entry&)>;
}  // namespace hades
