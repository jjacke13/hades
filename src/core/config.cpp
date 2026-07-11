// src/core/config.cpp — manifest parse + prompt assembly + version
//
// Merged (2026-07-04 src reorg): config/manifest (MOOS-style block parser, warnings,
// fatal multi-kv detection), config/prompt (assemble_system_prompt SOUL/USER +
// read_memory_layer live core memory), core/version.

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include "hades/config.h"
#include "hades/prompt.h"
#include "hades/version.h"
#include "hades/launcher.h"  // MalConfig

// ── MOOS-style block manifest parser (was src/config/manifest.cpp) ──────────────
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

// ── assemble_system_prompt (SOUL+USER) + read_memory_layer (live core memory) (was src/config/prompt.cpp) ──────────────
namespace hades {
namespace {
std::string read_or_throw(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw MalConfig("system prompt file not readable: " + path);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}
}  // namespace

std::string assemble_system_prompt(const Block& session) {
  static constexpr std::array<const char*, 2> kKeys = {"system_prompt_file", "user_file"};
  std::string out;
  for (const char* key : kKeys) {
    auto it = session.kv.find(key);
    if (it == session.kv.end() || it->second.empty()) continue;
    std::string content = read_or_throw(it->second);
    if (content.empty()) continue;
    if (!out.empty()) out += "\n\n";
    out += content;
  }
  return out;
}

std::string read_memory_layer(const std::string& path) {
  if (path.empty()) return "";
  std::ifstream f(path);
  if (!f) return "";  // core file may not exist until the first core_memory add — not an error
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}
}  // namespace hades

// ── hades semver string (was src/core/version.cpp) ──────────────
namespace hades { std::string version() { return "0.1.0"; } }
