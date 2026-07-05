// tools/grep_main.cpp — bundled grep native tool binary
//
// Reads one JSON line ({"call":"describe"|"grep","args":{pattern,path,ignore_case,context,
// max_results}}), regex-searches files under path (recursive; skips .git/.hades/build* dirs,
// binary files, files > 4 MB; does NOT follow directory symlinks), and returns matches as one
// JSON line. Read-only; the path argument is capability-gated upstream (FsRead scopes).
// Fail-closed: malformed input, bad regex, missing path -> ok:false, never throws.
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
namespace fs = std::filesystem;

namespace {
constexpr std::size_t kMaxFileBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxOutBytes = 64 * 1024;

bool skip_dir_name(const std::string& n) {
  return n == ".git" || n == ".hades" || n.rfind("build", 0) == 0;
}
bool looks_binary(std::ifstream& f) {   // NUL byte in the first 8 KB
  char buf[8192];
  f.read(buf, sizeof(buf));
  const std::streamsize got = f.gcount();
  for (std::streamsize i = 0; i < got; ++i)
    if (buf[i] == '\0') return true;
  f.clear();
  f.seekg(0);
  return false;
}

// Scan one file; append matches; returns false once caps are hit (stop the walk).
bool scan_file(const fs::path& p, const std::regex& re, long long context,
               long long max_results, std::size_t& out_bytes, nlohmann::json& matches) {
  std::error_code ec;
  if (fs::file_size(p, ec) > kMaxFileBytes || ec) return true;   // skip, keep walking
  std::ifstream f(p, std::ios::binary);
  if (!f || looks_binary(f)) return true;
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(f, line)) lines.push_back(line);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (!std::regex_search(lines[i], re)) continue;
    std::string text;
    const std::size_t lo = (context > 0 && i >= static_cast<std::size_t>(context))
                               ? i - static_cast<std::size_t>(context) : 0;
    const std::size_t hi = std::min(lines.size() - 1, i + static_cast<std::size_t>(context));
    for (std::size_t k = lo; k <= hi; ++k) {
      if (!text.empty()) text += "\n";
      text += std::to_string(k + 1) + ": " + lines[k];
    }
    const std::string& stored = context > 0 ? text : lines[i];
    matches.push_back({{"file", p.generic_string()},
                       {"line", static_cast<long long>(i + 1)},
                       {"text", stored}});
    out_bytes += stored.size() + p.generic_string().size() + 32;
    if (static_cast<long long>(matches.size()) >= max_results || out_bytes > kMaxOutBytes)
      return false;
  }
  return true;
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
            {{"name", "grep"},
             {"description",
              "Search file contents with a regular expression (ECMAScript). Returns matching "
              "lines as file/line/text. path may be a file or directory (searched recursively; "
              ".git and build dirs skipped). Read-only."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"pattern", {{"type", "string"}}},
                 {"path", {{"type", "string"}}},
                 {"ignore_case", {{"type", "boolean"}}},
                 {"context", {{"type", "integer"}}},
                 {"max_results", {{"type", "integer"}}}}},
               {"required", {"pattern"}}}}}}};
  } else if (call == "grep") {
    nlohmann::json args = (in.is_object() && in.contains("args") && in["args"].is_object())
                              ? in["args"] : nlohmann::json::object();
    auto jstr = [&](const char* k) {
      auto it = args.find(k);
      return (it != args.end() && it->is_string()) ? it->get<std::string>() : std::string{};
    };
    auto jbool = [&](const char* k, bool dflt) {
      auto it = args.find(k);
      return (it != args.end() && it->is_boolean()) ? it->get<bool>() : dflt;
    };
    auto jint = [&](const char* k, long long dflt) {
      auto it = args.find(k);
      return (it != args.end() && it->is_number_integer()) ? it->get<long long>() : dflt;
    };
    const std::string pattern = jstr("pattern");
    const std::string root = args.count("path") ? jstr("path") : std::string(".");
    const bool icase = jbool("ignore_case", false);
    const long long context = std::clamp(jint("context", 0), 0LL, 5LL);
    const long long max_results = std::clamp(jint("max_results", 100), 1LL, 500LL);
    std::error_code ec;
    if (pattern.empty()) {
      out = {{"ok", false}, {"result", {{"error", "missing arg: pattern"}}}};
    } else if (!fs::exists(root, ec) || ec) {
      out = {{"ok", false}, {"result", {{"error", "no such path: '" + root + "'"}}}};
    } else {
      try {
        auto flags = std::regex::ECMAScript;
        if (icase) flags |= std::regex::icase;
        const std::regex re(pattern, flags);
        nlohmann::json matches = nlohmann::json::array();
        std::size_t out_bytes = 0;
        bool more = true;
        if (fs::is_regular_file(root, ec)) {
          more = scan_file(root, re, context, max_results, out_bytes, matches);
        } else {
          fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
          for (; !ec && it != end && more; it.increment(ec)) {
            std::error_code dec;
            if (it->is_directory(dec) && !dec) {
              if (skip_dir_name(it->path().filename().string())) it.disable_recursion_pending();
              continue;
            }
            if (it->is_regular_file(dec) && !dec)
              more = scan_file(it->path(), re, context, max_results, out_bytes, matches);
          }
        }
        out = {{"ok", true}, {"result", {{"matches", matches}, {"truncated", !more}}}};
      } catch (const std::regex_error& e) {
        out = {{"ok", false}, {"result", {{"error", std::string("invalid regex: ") + e.what()}}}};
      } catch (...) {
        out = {{"ok", false}, {"result", {{"error", "grep failed"}}}};
      }
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }
  std::cout << out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << std::endl;
  return 0;
}
