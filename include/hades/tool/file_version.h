// include/hades/tool/file_version.h — content-hash version token for the staleness guard
//
// FNV-1a 64-bit over the raw bytes, rendered as 16 lowercase hex chars. fs_read stamps the bytes
// it returned; edit_file/write_file stamp the bytes they wrote and verify an Arbiter-injected
// expect_version before writing. Header-only so the standalone tool binaries share the exact same
// hash without linking hades_core. Detects ACCIDENTAL concurrent modification (lost updates) —
// not an adversary; collision resistance is a non-goal.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
namespace hades {
inline std::string file_version(const std::string& bytes) {
  std::uint64_t h = 14695981039346656037ULL;                // FNV offset basis
  for (unsigned char c : bytes) {
    h ^= c;
    h *= 1099511628211ULL;                                  // FNV prime
  }
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return buf;
}
}  // namespace hades
