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
