#pragma once
#include <cstddef>
#include <string>
#include <vector>
namespace hades {
struct ProcResult { int code; std::string out; std::string err; bool timed_out; };
ProcResult run_subprocess(const std::vector<std::string>& argv,
                          const std::string& stdin_data,
                          double timeout_s,
                          std::size_t mem_limit_mb = 0);   // RLIMIT_AS in child if >0
}  // namespace hades
