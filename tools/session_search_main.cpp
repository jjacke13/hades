// tools/session_search_main.cpp — bundled session_search native tool binary
//
// Explicit full-text recall over PAST sessions: reads one JSON line
// ({"call":"describe"|"session_search","args":{query, max_results?}}), splits every
// <sessions_dir>/*.jsonl into per-turn "U:…\nA:…" units (extract_session_turns, compiled in via
// src/core/session.cpp — no core link) and ranks them by lowercased token overlap with the
// query (the rank_memories idiom). argv[1] = sessions dir (wiring-pinned; fallback
// ".hades/sessions"). The live-session FILENAME to exclude (the Arbiter already holds that
// context in-history) arrives as the `exclude_session` ARG — Arbiter-injected at dispatch and
// stripped of any LLM-supplied value, the expect_version pattern. It is deliberately NOT in argv
// any more: a session is a DAY, and a filename pinned at launch is wrong from the first rollover
// on (it would skip yesterday and rank today's live file).
// Complements the auto-injected embedding recall: this is the
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
  std::string session;  // file stem
  std::size_t turn;
  std::string text;
  // Two id shapes coexist on disk (legacy "20260601-101010" and daily "2026-09-06"), and
  // comparing stems ranks EVERY legacy session above EVERY daily one: at index 4 a date has "-"
  // (0x2D) and a stamp has a digit (0x30+). Scores here are small token-overlap counts, so ties
  // are the common case, and with a few legacy sessions the current day falls off the end of
  // max_results entirely. Order by mtime instead — the same fix, for the same reason, as
  // resolve_session_path's newest-session branch in src/core/session.cpp; keep the two together.
  std::filesystem::file_time_type mtime;
};
}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : ".hades/sessions";
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
      // Arbiter-injected live-session filename (see the header comment). Absent/non-string =
      // exclude nothing, which is exactly the pre-injection behaviour of an unset live session.
      const std::string live = (args.contains("exclude_session") &&
                                args["exclude_session"].is_string())
                                   ? args["exclude_session"].get<std::string>()
                                   : "";
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
        std::error_code mec;
        auto mtime = it->last_write_time(mec);
        if (mec) mtime = std::filesystem::file_time_type::min();   // unreadable -> sorts oldest
        std::size_t idx = 0;
        for (const auto& t : hades::extract_session_turns(it->path().string())) {
          const auto utok = tokens_of(t.text);
          std::size_t score = 0;
          for (const auto& q : qtok)
            if (utok.count(q)) ++score;
          if (score > 0) {
            std::string text = t.text.substr(0, kUnitTruncate);
            hits.push_back({score, stem, idx, std::move(text), mtime});
          }
          ++idx;
        }
      }
      std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.score != b.score) return a.score > b.score;      // best overlap first
        if (a.mtime != b.mtime) return a.mtime > b.mtime;      // newer session first (by mtime)
        if (a.session != b.session) return a.session > b.session;  // determinism on equal mtime
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
  // UTF-8-replace dump (house pattern, every content-echoing tool): the 700-byte truncation
  // can split a multibyte codepoint in real session text, and a strict dump() would THROW on
  // the invalid tail — the one-JSON-line contract must hold on valid (non-ASCII) sessions.
  std::cout << out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << std::endl;
  return 0;
}
