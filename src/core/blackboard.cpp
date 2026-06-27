// src/core/blackboard.cpp — Blackboard pub/sub store implementation
//
// Implements the FIFO event queue, latest-value map, and pattern-matched subscription
// dispatch (exact / PREFIX* / *). post() appends each Entry to the Eventlog and
// enqueues it; pump() drains the queue and fires matching handlers with optional
// min-interval rate-limiting. Pimpl (Impl) holds the Eventlog pointer and sub list.

#include "hades/blackboard.h"
#include "hades/eventlog.h"
#include <chrono>
#include <deque>
namespace hades {
namespace { double mono(){ using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count(); } }

struct Sub { std::string pattern; Handler h; double min_interval; std::map<std::string,double> last; };

struct Blackboard::Impl {
  Eventlog* log; double t0; std::uint64_t seq=0;
  std::map<std::string,Entry> latest;
  std::vector<Sub> subs;
  std::deque<Entry> queue;
};

static bool match(const std::string& pat, const std::string& key){
  if(pat=="*") return true;
  if(!pat.empty() && pat.back()=='*') return key.compare(0, pat.size()-1, pat, 0, pat.size()-1)==0;
  return pat==key;
}
Blackboard::Blackboard(Eventlog* log) : p_(std::make_unique<Impl>()) { p_->log=log; p_->t0=mono(); }
Blackboard::~Blackboard() = default;
double Blackboard::now() const { return mono() - p_->t0; }
void Blackboard::subscribe(const std::string& pattern, Handler h, double mi){
  p_->subs.push_back({pattern, std::move(h), mi, {}});
}
void Blackboard::post(const std::string& key, nlohmann::json value,
                      const std::string& source, const std::string& aux){
  Entry e{key, std::move(value), source, aux, now(), ++p_->seq};
  p_->latest[key] = e;
  if(p_->log) p_->log->append(e);
  p_->queue.push_back(std::move(e));
}
std::optional<Entry> Blackboard::get(const std::string& key) const {
  auto it=p_->latest.find(key); if(it==p_->latest.end()) return std::nullopt; return it->second;
}
std::size_t Blackboard::queued() const { return p_->queue.size(); }
void Blackboard::pump(){
  while(!p_->queue.empty()){
    Entry e = p_->queue.front(); p_->queue.pop_front();
    for(auto& s : p_->subs){
      if(!match(s.pattern, e.key)) continue;
      double last = s.last.count(e.key) ? s.last[e.key] : -1e18;
      if(s.min_interval>0 && e.ts-last < s.min_interval) continue;
      s.last[e.key]=e.ts;
      s.h(e);
    }
  }
}
}  // namespace hades
