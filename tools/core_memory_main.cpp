// tools/core_memory_main.cpp — bundled core_memory native tool binary
//
// Bounded, editable core memory: reads one JSON line
// ({"call":"describe"|"core_memory","args":{action,text,match}}) and edits the always-on
// core-memory file. argv (fixed by wiring, never LLM-chosen): [1] file (fallback
// "memory/facts.md"), [2] char cap (fallback/garbage/<=0 -> kDefaultMemoryCharLimit).
// Every non-empty line is an entry; add/replace write canonical "- <text>" bullets (newlines
// fold to spaces — one entry, one line). An add/replace that would push the file over the cap
// fails with a NUMBERED entry list so the model consolidates IN THE SAME TURN (the Hermes
// forcing function). Writes are atomic (temp+rename). Fail-closed: malformed/adversarial
// input returns ok:false, never throws, never partial-writes. Empty-string args = absent.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "hades/memory_limit.h"   // kDefaultMemoryCharLimit (header-only; no core link)

using nlohmann::json;
namespace fs = std::filesystem;

static std::vector<std::string> read_lines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream f(path);
  std::string l;
  while (std::getline(f, l)) lines.push_back(l);
  return lines;
}

static std::string join_lines(const std::vector<std::string>& lines) {
  std::string out;
  for (const auto& l : lines) { out += l; out += '\n'; }
  return out;
}

// Numbered non-empty entries for the consolidation error.
static std::string list_entries(const std::vector<std::string>& lines) {
  std::string out;
  int n = 0;
  for (const auto& l : lines)
    if (!l.empty()) out += std::to_string(++n) + ". " + l + "\n";
  return out;
}

int main(int argc, char** argv) {
  const std::string file = argc > 1 ? argv[1] : "memory/facts.md";
  errno = 0;
  long long cap = argc > 2 ? std::strtoll(argv[2], nullptr, 10) : hades::kDefaultMemoryCharLimit;
  if (errno == ERANGE || cap <= 0) cap = hades::kDefaultMemoryCharLimit;   // garbage/overflow/misconfig must not brick memory

  std::string line;
  std::getline(std::cin, line);
  auto in = json::parse(line, nullptr, false);

  std::string call;
  if (in.is_object() && in.contains("call") && in["call"].is_string()) call = in["call"].get<std::string>();

  if (call == "describe") {
    json out = {{"ok", true},
                {"result",
                 {{"name", "core_memory"},
                  {"description",
                   "Edit your always-on core memory (in your context every turn). action=add "
                   "with text: pin a new standing fact (identity/preferences/facts you always "
                   "need — use save_memory instead for details to recall later). action=replace "
                   "with match+text: rewrite ONE existing entry (match = substring identifying "
                   "it uniquely). action=remove with match: delete ONE entry. Memory is capped; "
                   "when full the error lists every entry — consolidate (merge with replace, "
                   "drop with remove), then retry."},
                  {"schema",
                   {{"type", "object"},
                    {"properties",
                     {{"action", {{"type", "string"}, {"enum", {"add", "replace", "remove"}}}},
                      {"text", {{"type", "string"}}},
                      {"match", {{"type", "string"}}}}},
                    {"required", json::array({"action"})}}}}}};
    std::cout << out.dump() << std::endl;
    return 0;
  }
  auto fail = [&](const std::string& e) {
    std::cout << json{{"ok", false}, {"result", {{"error", e}}}}.dump() << std::endl;
    return 0;
  };
  if (call != "core_memory") return fail("unknown call: " + call);

  json args = (in.is_object() && in.contains("args") && in["args"].is_object()) ? in["args"] : json::object();
  auto str = [&](const char* k) {
    return args.contains(k) && args[k].is_string() ? args[k].get<std::string>() : std::string{};
  };
  const std::string action = str("action");
  std::string text = str("text");
  const std::string match = str("match");
  for (char& c : text)
    if (c == '\n' || c == '\r') c = ' ';   // one entry = one line; no injected structure

  // Empty string = absent (the exactly-one-of lesson): validate per action.
  if (action != "add" && action != "replace" && action != "remove")
    return fail("action must be one of: add, replace, remove");
  if ((action == "add" || action == "replace") && text.empty()) return fail("missing arg: text");
  if ((action == "replace" || action == "remove") && match.empty()) return fail("missing arg: match");

  std::vector<std::string> lines = read_lines(file);
  const std::string entry = "- " + text;

  if (action == "add") {
    for (const auto& l : lines)
      if (l == entry) return fail("already pinned: " + entry);
    lines.push_back(entry);
  } else {
    // Substring match over non-empty lines; exactly one hit or fail-closed (never guess).
    std::vector<std::size_t> hits;
    for (std::size_t i = 0; i < lines.size(); ++i)
      if (!lines[i].empty() && lines[i].find(match) != std::string::npos) hits.push_back(i);
    if (hits.empty()) return fail("no entry matches: " + match);
    if (hits.size() > 1) {
      std::string e = "match is ambiguous (" + std::to_string(hits.size()) + " entries):\n";
      for (auto i : hits) e += lines[i] + "\n";
      return fail(e + "give a longer match that identifies exactly one");
    }
    if (action == "replace") lines[hits[0]] = entry;
    else lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(hits[0]));
  }

  const std::string content = join_lines(lines);
  if ((action == "add" || action == "replace") &&
      static_cast<long long>(content.size()) > cap) {
    // The forcing function: refuse, and hand back everything needed to consolidate NOW.
    return fail("core memory full: this write would make it " + std::to_string(content.size()) +
                "/" + std::to_string(cap) + " chars. Entries:\n" + list_entries(read_lines(file)) +
                "Consolidate: merge or drop entries with replace/remove, then retry — or "
                "shorten the text if it alone exceeds the cap.");
  }

  fs::path p(file);
  if (p.has_parent_path()) { std::error_code ec; fs::create_directories(p.parent_path(), ec); }
  const std::string tmp = file + ".tmp";
  std::ofstream f(tmp, std::ios::trunc);
  if (f) { f << content; f.close(); }
  if (!f) { std::remove(tmp.c_str()); return fail("cannot write: " + file); }
  std::error_code ec;
  fs::rename(tmp, file, ec);   // atomic on POSIX; replaces existing
  if (ec) { std::remove(tmp.c_str()); return fail("cannot save: " + file); }

  int entries = 0;
  for (const auto& l : lines) if (!l.empty()) ++entries;
  json result{{"action", action}, {"entries", entries},
              {"chars", static_cast<long long>(content.size())}, {"cap", cap}};
  std::cout << json{{"ok", true}, {"result", result}}.dump() << std::endl;
  return 0;
}
