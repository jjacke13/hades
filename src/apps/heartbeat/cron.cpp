// src/apps/heartbeat/cron.cpp — 5-field cron parse + match (pure, tolerant, never throws)
#include "hades/heartbeat/cron.h"
#include <sstream>
#include <vector>
namespace hades {
namespace {

// Parse a decimal int from [b,e). Returns false on empty/non-digit/overflow.
bool parse_int(const std::string& s, std::size_t b, std::size_t e, int& out) {
  if (b >= e) return false;
  long v = 0;
  for (std::size_t i = b; i < e; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
    if (v > 100000) return false;
  }
  out = static_cast<int>(v);
  return true;
}

// Match one comma-less term (spec) for value v in [lo,hi]. Grammar: '*' | '*/N' | A | A-B | A-B/N.
// valid_out (optional) reports whether the term is well-formed even when it doesn't match v.
bool term_match(const std::string& spec, int v, int lo, int hi, bool& valid) {
  valid = false;
  if (spec.empty()) return false;
  // split off "/step"
  int step = 1;
  std::string base = spec;
  if (auto slash = spec.find('/'); slash != std::string::npos) {
    if (!parse_int(spec, slash + 1, spec.size(), step) || step <= 0) return false;
    base = spec.substr(0, slash);
  }
  int a = lo, b = hi;
  if (base == "*") {
    // a..b already lo..hi
  } else if (auto dash = base.find('-'); dash != std::string::npos) {
    if (!parse_int(base, 0, dash, a) || !parse_int(base, dash + 1, base.size(), b)) return false;
  } else {
    if (!parse_int(base, 0, base.size(), a)) return false;
    b = a;
  }
  if (a < lo || b > hi || a > b) return false;
  valid = true;                              // well-formed within range
  if (v < a || v > b) return false;
  return ((v - a) % step) == 0;
}

// Match a whole field (comma list) for value v in [lo,hi]. valid = every term well-formed.
bool field_match(const std::string& field, int v, int lo, int hi, bool& valid) {
  valid = true;
  bool any = false;
  std::stringstream ss(field);
  std::string term;
  bool saw_term = false;
  while (std::getline(ss, term, ',')) {
    saw_term = true;
    bool tv = false;
    if (term_match(term, v, lo, hi, tv)) any = true;
    if (!tv) valid = false;
  }
  if (!saw_term) valid = false;
  return any;
}

// Split into exactly 5 whitespace-separated fields. Returns false otherwise.
bool split5(const std::string& expr, std::vector<std::string>& out) {
  out.clear();
  std::stringstream ss(expr);
  std::string f;
  while (ss >> f) out.push_back(f);
  return out.size() == 5;
}

}  // namespace

bool cron_matches(const std::string& expr, const std::tm& t) {
  std::vector<std::string> f;
  if (!split5(expr, f)) return false;
  const int vals[5] = {t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon + 1, t.tm_wday};
  const int lo[5]   = {0, 0, 1, 1, 0};
  const int hi[5]   = {59, 23, 31, 12, 6};
  for (int i = 0; i < 5; ++i) {
    bool valid = false;
    if (!field_match(f[i], vals[i], lo[i], hi[i], valid)) return false;
  }
  return true;
}

bool cron_valid(const std::string& expr) {
  std::vector<std::string> f;
  if (!split5(expr, f)) return false;
  const int lo[5] = {0, 0, 1, 1, 0};
  const int hi[5] = {59, 23, 31, 12, 6};
  for (int i = 0; i < 5; ++i) {
    bool valid = false;
    field_match(f[i], lo[i], lo[i], hi[i], valid);   // value irrelevant; we only want well-formedness
    if (!valid) return false;
  }
  return true;
}
}  // namespace hades
