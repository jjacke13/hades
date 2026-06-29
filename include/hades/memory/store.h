// include/hades/memory/store.h — append-only JSONL memory store reader
//
// load_memories: parse one JSON object per line ({"text","ts"}). Missing file ->
// empty (a fresh agent). Malformed or text-less lines are skipped, never thrown.
#pragma once
#include <string>
#include <vector>
#include "hades/memory/record.h"
namespace hades {
std::vector<MemoryRecord> load_memories(const std::string& path);
}  // namespace hades
