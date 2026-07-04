// tools/glob_main.cpp — bundled glob native tool binary
//
// Reads one JSON line ({"call":"describe"|"glob","args":{pattern,path,max_results}}), finds
// files under path whose RELATIVE path matches the glob (*, ? within a segment; ** across
// segments), and returns the sorted list as one JSON line. Same walk rules as grep (skips
// .git/.hades/build* dirs, no symlink-dir follow). Read-only; path capability-gated upstream.
// Fail-closed: malformed input, missing path -> ok:false, never throws.
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <regex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
namespace fs = std::filesystem;

namespace {
bool skip_dir_name(const std::string& n) {
  return n == ".git" || n == ".hades" || n.rfind("build", 0) == 0;
}

// Translate a glob into an anchored ECMAScript regex over the RELATIVE generic path.
// "**/" -> "(?:.*/)?"   "**" -> ".*"   "*" -> "[^/]*"   "?" -> "[^/]"   else escaped.
std::string glob_to_regex(const std::string& g) {
  std::string re = "^";
  for (std::size_t i = 0; i < g.size();) {
    if (g.compare(i, 3, "**/") == 0) { re += "(?:.*/)?"; i += 3; }
    else if (g.compare(i, 2, "**") == 0) { re += ".*"; i += 2; }
    else if (g[i] == '*') { re += "[^/]*"; ++i; }
    else if (g[i] == '?') { re += "[^/]"; ++i; }
    else {
      if (std::string("\\^$.|+()[]{}").find(g[i]) != std::string::npos) re += '\\';
      re += g[i];
      ++i;
    }
  }
  return re + "$";
}
}  // namespace

int main() {
  std::string line;
  std::getline(std::cin, line);
  auto in = nlohmann::json::parse(line, nullptr, false);

  nlohmann::json out;
  std::string call;
  if (in.is_object() && in.contains("call") && in["call"].is_string())
    call = in["call"].get<std::string>();

  if (call == "describe") {
    out = {{"ok", true},
           {"result",
            {{"name", "glob"},
             {"description",
              "Find files by glob pattern relative to path (default .): * and ? within a path "
              "segment, ** across segments (e.g. **/*.cpp). Sorted. Read-only."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"pattern", {{"type", "string"}}},
                 {"path", {{"type", "string"}}},
                 {"max_results", {{"type", "integer"}}}}},
               {"required", {"pattern"}}}}}}};
  } else if (call == "glob") {
    nlohmann::json args = (in.is_object() && in.contains("args") && in["args"].is_object())
                              ? in["args"] : nlohmann::json::object();
    auto jstr = [&](const char* k) {
      auto it = args.find(k);
      return (it != args.end() && it->is_string()) ? it->get<std::string>() : std::string{};
    };
    auto jint = [&](const char* k, long long dflt) {
      auto it = args.find(k);
      return (it != args.end() && it->is_number_integer()) ? it->get<long long>() : dflt;
    };
    const std::string pattern = jstr("pattern");
    const std::string root = args.count("path") ? jstr("path") : std::string(".");
    const long long max_results = std::clamp(jint("max_results", 200), 1LL, 1000LL);
    std::error_code ec;
    if (pattern.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: pattern"}}}};
    } else if (!fs::is_directory(root, ec) || ec) {
      out = {{"ok", false}, {"result", {{"error", "no such directory: " + root}}}};
    } else {
      try {
        const std::regex re(glob_to_regex(pattern));
        std::vector<std::string> files;
        bool truncated = false;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
          std::error_code dec;
          if (it->is_directory(dec) && !dec) {
            if (skip_dir_name(it->path().filename().string())) it.disable_recursion_pending();
            continue;
          }
          if (!it->is_regular_file(dec) || dec) continue;
          const std::string rel = fs::relative(it->path(), root, dec).generic_string();
          if (dec || !std::regex_match(rel, re)) continue;
          if (static_cast<long long>(files.size()) >= max_results) { truncated = true; break; }
          files.push_back(it->path().generic_string());
        }
        std::sort(files.begin(), files.end());
        out = {{"ok", true}, {"result", {{"files", files}, {"truncated", truncated}}}};
      } catch (...) {
        out = {{"ok", false}, {"result", {{"error", "glob failed"}}}};
      }
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }
  std::cout << out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << std::endl;
  return 0;
}
