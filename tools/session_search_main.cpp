// tools/session_search_main.cpp — bundled session_search native tool binary
//
// Explicit full-text recall over PAST sessions: reads one JSON line
// ({"call":"describe"|"session_search","args":{query, max_results?}}), splits every
// <sessions_dir>/*.jsonl into per-turn "U:…\nA:…" units (extract_session_turns, compiled in via
// src/core/session.cpp — no core link) and ranks them by lowercased token overlap with the
// query (the rank_memories idiom). argv[1] = sessions dir (wiring-pinned; fallback
// ".hades/sessions"), argv[2] = live-session FILENAME to exclude (the Arbiter already holds
// that context in-history). Complements the auto-injected embedding recall: this is the
// deliberate, exact "did we discuss X?" path. Raw excerpts only — summarizing is the caller's
// job. Fail-closed on malformed input; no hits is ok:true with an empty list, not an error.
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "hades/embedding/session_turns.h"   // extract_session_turns (impl: src/core/session.cpp)

namespace {
constexpr std::size_t kDefaultResults = 5;
constexpr std::size_t kMaxResults     = 20;
constexpr std::size_t kUnitTruncate   = 700;

std::set<std::string> tokens_of(const std::string& s) {
  std::set<std::string> out;
  std::string cur;
  for (char c : s) {
    if (std::isalnum(static_cast<unsigned char>(c)))
      cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    else if (!cur.empty()) { out.insert(cur); cur.clear(); }
  }
  if (!cur.empty()) out.insert(cur);
  return out;
}

struct Hit {
  std::size_t score;
  std::string session;  // file stem — timestamp ids sort lexically = chronologically
  std::size_t turn;
  std::string text;
};
}  // namespace

int main(int argc, char** argv) {
  const std::string dir  = argc > 1 ? argv[1] : ".hades/sessions";
  const std::string live = argc > 2 ? argv[2] : "";
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
            {{"name", "session_search"},
             {"description",
              "Search your PAST conversation sessions by keywords and get back the matching "
              "user/assistant exchanges verbatim. Use it to answer \"did we discuss X?\" or to "
              "recover details the automatic memory recall did not surface. The current "
              "conversation is not searched (you already have it)."},
             {"schema",
              {{"type", "object"},
               {"properties",
                {{"query", {{"type", "string"}}},
                 {"max_results",
                  {{"type", "integer"},
                   {"description", "how many excerpts to return (default 5, max 20)"}}}}},
               {"required", {"query"}}}}}}};
  } else if (call == "session_search") {
    nlohmann::json args = (in.is_object() && in.contains("args") && in["args"].is_object())
                              ? in["args"]
                              : nlohmann::json::object();
    const bool has_q = args.contains("query") && args["query"].is_string();
    const std::string query = has_q ? args["query"].get<std::string>() : "";
    const auto qtok = tokens_of(query);
    if (query.empty() || qtok.empty()) {                 // empty = absent (house rule)
      out = {{"ok", false}, {"result", {{"error", "missing arg: query (non-empty keywords)"}}}};
    } else {
      std::size_t max_results = kDefaultResults;
      if (args.contains("max_results") && args["max_results"].is_number_integer()) {
        const long long m = args["max_results"].get<long long>();
        if (m > 0) max_results = std::min<std::size_t>(static_cast<std::size_t>(m), kMaxResults);
      }
      std::vector<Hit> hits;
      int searched = 0;
      std::error_code ec;
      std::filesystem::directory_iterator it(dir, ec), end;
      for (; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) continue;
        if (it->path().extension() != ".jsonl") continue;
        if (!live.empty() && it->path().filename().string() == live) continue;  // live session
        ++searched;
        const std::string stem = it->path().stem().string();
        std::size_t idx = 0;
        for (const auto& t : hades::extract_session_turns(it->path().string())) {
          const auto utok = tokens_of(t.text);
          std::size_t score = 0;
          for (const auto& q : qtok)
            if (utok.count(q)) ++score;
          if (score > 0) {
            std::string text = t.text.substr(0, kUnitTruncate);
            hits.push_back({score, stem, idx, std::move(text)});
          }
          ++idx;
        }
      }
      std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.score != b.score) return a.score > b.score;      // best overlap first
        if (a.session != b.session) return a.session > b.session;  // newer session first
        return a.turn > b.turn;                                // later turn first
      });
      if (hits.size() > max_results) hits.resize(max_results);
      nlohmann::json jhits = nlohmann::json::array();
      for (const auto& h : hits)
        jhits.push_back({{"session", h.session}, {"turn", h.turn}, {"text", h.text}});
      out = {{"ok", true},
             {"result", {{"hits", jhits}, {"searched_sessions", searched}}}};
    }
  } else {
    out = {{"ok", false}, {"result", {{"error", "unknown call: " + call}}}};
  }
  std::cout << out.dump() << std::endl;
  return 0;
}
