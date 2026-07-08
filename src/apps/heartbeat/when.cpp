// src/apps/heartbeat/when.cpp — parse + evaluate the reactive when-condition (pure, tolerant)
#include "hades/heartbeat/when.h"
#include <cmath>
#include <cstdlib>
namespace hades {
namespace {
// The value as the string is/not compare against: raw content for a JSON string, compact dump else.
std::string value_text(const nlohmann::json& v) {
  return v.is_string() ? v.get<std::string>() : v.dump();
}
bool parse_number(const std::string& s, double& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  out = std::strtod(s.c_str(), &end);
  // A threshold must be a finite number: "nan"/"inf" parse but make every compare silently false
  // (or never-fire) — reject loud at validation time instead.
  return end && *end == '\0' && std::isfinite(out);
}
}  // namespace

std::optional<WhenCond> parse_when(const std::string& expr) {
  const std::size_t sp1 = expr.find(' ');
  if (sp1 == std::string::npos || sp1 == 0) return std::nullopt;
  WhenCond c;
  c.key = expr.substr(0, sp1);
  const std::size_t op_start = expr.find_first_not_of(' ', sp1);
  if (op_start == std::string::npos) return std::nullopt;
  const std::size_t sp2 = expr.find(' ', op_start);
  const std::string op = expr.substr(op_start, sp2 == std::string::npos ? std::string::npos
                                                                        : sp2 - op_start);
  std::string rest;
  if (sp2 != std::string::npos) {
    const std::size_t rest_start = expr.find_first_not_of(' ', sp2);
    if (rest_start != std::string::npos) rest = expr.substr(rest_start);
  }
  // Trim trailing whitespace: "K is idle " must match "idle", and "K above 0.8 " must validate —
  // the LLM-supplied dynamic path (schedule_task) can carry a sloppy trailing space.
  while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t')) rest.pop_back();
  if (op == "changes") {
    if (!rest.empty()) return std::nullopt;              // changes takes no operand
    c.op = WhenCond::Op::Changes;
    return c;
  }
  if (rest.empty()) return std::nullopt;                 // every other op needs an operand
  c.operand = rest;
  if (op == "is")  { c.op = WhenCond::Op::Is;  return c; }
  if (op == "not") { c.op = WhenCond::Op::Not; return c; }
  double n;
  if (op == "above") { c.op = WhenCond::Op::Above; return parse_number(rest, n) ? std::optional(c) : std::nullopt; }
  if (op == "below") { c.op = WhenCond::Op::Below; return parse_number(rest, n) ? std::optional(c) : std::nullopt; }
  return std::nullopt;                                   // unknown op
}

bool when_valid(const std::string& expr) { return parse_when(expr).has_value(); }

bool when_holds(const WhenCond& c, const nlohmann::json* value) {
  if (!value) return false;                              // absent key: condition cannot hold
  switch (c.op) {
    case WhenCond::Op::Is:  return value_text(*value) == c.operand;
    case WhenCond::Op::Not: return value_text(*value) != c.operand;
    case WhenCond::Op::Above:
    case WhenCond::Op::Below: {
      if (!value->is_number()) return false;
      const double threshold = std::strtod(c.operand.c_str(), nullptr);   // validated at parse time
      const double v = value->get<double>();
      return c.op == WhenCond::Op::Above ? v > threshold : v < threshold;
    }
    case WhenCond::Op::Changes: return false;            // stateful; module-evaluated
  }
  return false;
}
}  // namespace hades
