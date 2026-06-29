// include/hades/memory/record.h — one persisted memory: free text + save timestamp
#pragma once
#include <string>
namespace hades {
struct MemoryRecord { std::string text; double ts = 0.0; };
}  // namespace hades
