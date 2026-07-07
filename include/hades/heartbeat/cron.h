// include/hades/heartbeat/cron.h — pure 5-field cron matcher (min hour dom month dow)
//
// Evaluates a standard 5-field cron expression against a std::tm (machine-LOCAL time). Each field
// supports '*', N, A-B, A-B/N, '*/N', and comma lists (A,B,C). Fields are ANDed (the Vixie dom/dow
// OR quirk is intentionally NOT implemented). Tolerant: any parse failure or a field count != 5
// yields no-match (cron_matches) / invalid (cron_valid) — never throws. Minute resolution.
#pragma once
#include <ctime>
#include <string>
namespace hades {
bool cron_matches(const std::string& expr, const std::tm& t);
bool cron_valid(const std::string& expr);
}  // namespace hades
