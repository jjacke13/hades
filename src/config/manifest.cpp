// src/config/manifest.cpp — MOOS-style block manifest parser implementation
//
// Implements parse_manifest(): tokenizes plain-text "Section = name { key = value }"
// input into a Manifest of Block structs; collects warnings, never throws. Provides
// Manifest::session() and Manifest::of() accessors plus string-to-typed-value helpers
// (set_double_on_string, set_bool_on_string) used by Launcher and module on_start() handlers.

#include "hades/config.h"
#include <algorithm>
#include <cctype>
#include <sstream>
namespace hades {

static std::string trim(std::string s) {
  auto ns = [](int c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), ns));
  s.erase(std::find_if(s.rbegin(), s.rend(), ns).base(), s.end());
  return s;
}

static std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

static bool is_ident_start(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
static bool is_ident(char c)       { return std::isalnum((unsigned char)c) || c == '_'; }

// True if `value` (the substring AFTER a line's first '=') packs a second
// "<whitespace><ident><ws?>=" pair. The REQUIRED leading whitespace is the discriminator:
// a '=' inside a single token (URL query "?a=b", base64 padding "x==") is never preceded
// by whitespace+identifier, so those never match — only a genuine second key does.
static bool packs_second_kv(const std::string& value) {
  for (std::size_t i = 0; i + 1 < value.size(); ++i) {
    if (!std::isspace((unsigned char)value[i])) continue;          // need whitespace first
    std::size_t j = i;
    while (j < value.size() && std::isspace((unsigned char)value[j])) ++j;   // skip ws
    if (j >= value.size() || !is_ident_start(value[j])) continue;  // need an identifier
    std::size_t k = j + 1;
    while (k < value.size() && is_ident(value[k])) ++k;            // consume identifier
    while (k < value.size() && std::isspace((unsigned char)value[k])) ++k;   // optional ws
    if (k < value.size() && value[k] == '=') return true;          // identifier then '='
  }
  return false;
}

static void split_kv(const std::string& line, Block& b, Manifest& m) {
  auto eq = line.find('=');
  if (eq == std::string::npos) {
    m.warnings.push_back("bad config line: " + line);
    return;
  }
  std::string value = trim(line.substr(eq + 1));
  if (packs_second_kv(value))
    m.warnings.push_back(std::string(kMultiKvWarning) +
                         " (only one per line; use a multi-line { } block): " + line);
  b.kv[lower(trim(line.substr(0, eq)))] = value;
}

static void parse_header(const std::string& h, Block& b) {
  auto eq = h.find('=');
  if (eq == std::string::npos) {
    b.section = trim(h);
    b.name = "";
  } else {
    b.section = trim(h.substr(0, eq));
    b.name = trim(h.substr(eq + 1));
  }
}

Manifest parse_manifest(const std::string& text) {
  Manifest m;
  std::istringstream in(text);
  std::string raw;
  Block* open = nullptr;

  while (std::getline(in, raw)) {
    // Strip comment to end of line
    if (auto h = raw.find('#'); h != std::string::npos) raw = raw.substr(0, h);
    std::string line = trim(raw);
    if (line.empty()) continue;

    if (open) {
      // Inside a multi-line {...} body
      if (line == "}") {
        open = nullptr;
        continue;
      }
      split_kv(line, *open, m);
      continue;
    }

    if (line == "}") { m.warnings.push_back("unexpected '}' outside block"); continue; }

    // Bare opening brace on its own line attaches to most recent block
    if (line == "{") {
      if (!m.blocks.empty()) open = &m.blocks.back();
      continue;
    }

    // Header (possibly with inline braces: Section = name { key = value })
    auto ob = line.find('{');
    Block b;
    if (ob != std::string::npos) {
      parse_header(line.substr(0, ob), b);
      auto cb = line.find('}', ob);
      std::string body = (cb == std::string::npos)
                           ? line.substr(ob + 1)
                           : line.substr(ob + 1, cb - ob - 1);
      m.blocks.push_back(b);
      if (!trim(body).empty()) split_kv(trim(body), m.blocks.back(), m);
      // If no closing brace found, body continues on subsequent lines
      if (cb == std::string::npos) open = &m.blocks.back();
    } else {
      // Header-only or pre-brace (bare next line will open)
      parse_header(line, b);
      m.blocks.push_back(b);
    }
  }
  if (open) m.warnings.push_back("unclosed '{' block: " + open->section);
  return m;
}

std::vector<std::string> fatal_warnings(const Manifest& m) {
  std::vector<std::string> out;
  for (const auto& w : m.warnings)
    if (w.starts_with(kMultiKvWarning)) out.push_back(w);
  return out;
}

std::optional<Block> Manifest::session() const {
  for (const auto& b : blocks)
    if (lower(b.section) == "session") return b;
  return std::nullopt;
}

std::vector<Block> Manifest::of(const std::string& s) const {
  std::vector<Block> r;
  for (const auto& b : blocks)
    if (lower(b.section) == lower(s)) r.push_back(b);
  return r;
}

bool set_double_on_string(const std::string& v, double& out) {
  try {
    std::size_t i = 0;
    double d = std::stod(v, &i);
    if (i != v.size()) return false;
    out = d;
    return true;
  } catch (...) {
    return false;
  }
}

bool set_pos_double_on_string(const std::string& v, double& out) {
  double d = 0.0;
  if (!set_double_on_string(v, d) || d <= 0) return false;
  out = d;
  return true;
}

bool set_bool_on_string(const std::string& v, bool& out) {
  std::string l = lower(v);
  if (l == "true" || l == "1") { out = true;  return true; }
  if (l == "false" || l == "0") { out = false; return true; }
  return false;
}

}  // namespace hades
