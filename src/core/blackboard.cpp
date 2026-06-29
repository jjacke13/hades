// src/core/blackboard.cpp — Blackboard pub/sub store implementation
//
// Implements the FIFO event queue, latest-value map, and pattern-matched subscription
// dispatch (exact / PREFIX* / *). post() appends each Entry to the Eventlog and
// enqueues it; pump() drains the queue and fires matching handlers with optional
// min-interval rate-limiting. Pimpl (Impl) holds the Eventlog pointer and sub list.

#include "hades/blackboard.h"
#include "hades/eventlog.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
namespace hades {
namespace { double mono(){ using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count(); } }

struct Sub { std::string pattern; Handler h; double min_interval; std::map<std::string,double> last; };

struct Blackboard::Impl {
  Eventlog* log; double t0; std::uint64_t seq=0;
  std::map<std::string,Entry> latest;
  std::vector<Sub> subs;             // mutated only on the pump thread (subscribe/dispatch)
  std::deque<Entry> queue;           // guarded by mu (post from any thread, pump drains)
  std::mutex mu;                     // guards seq/latest/queue (the cross-thread state)
  std::condition_variable cv;        // signalled by post(), waited on by run_until()
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
  // Thread-safe: may be called from worker threads. Mutate shared state under
  // the lock, then notify outside it so a woken run_until() doesn't immediately
  // contend on a still-held mutex. ts is read INSIDE the lock so it stays
  // monotonic with seq/queue/eventlog order under concurrent cross-thread posts
  // (otherwise pump()'s min_interval could see a negative e.ts-last and skip).
  {
    std::lock_guard<std::mutex> lk(p_->mu);
    const double ts = now();
    Entry e{key, std::move(value), source, aux, ts, ++p_->seq};
    p_->latest[key] = e;
    if(p_->log) p_->log->append(e);
    p_->queue.push_back(std::move(e));
  }
  p_->cv.notify_one();
}
std::optional<Entry> Blackboard::get(const std::string& key) const {
  std::lock_guard<std::mutex> lk(p_->mu);
  auto it=p_->latest.find(key); if(it==p_->latest.end()) return std::nullopt; return it->second;
}
std::size_t Blackboard::queued() const {
  std::lock_guard<std::mutex> lk(p_->mu);
  return p_->queue.size();
}
void Blackboard::pump(){
  // Handlers run ONLY here (the pump thread). Pop each Entry under the lock,
  // then UNLOCK before dispatch — a handler's own post() re-locks mu, so holding
  // it across s.h(e) would self-deadlock. subs are touched only on this thread.
  for(;;){
    std::unique_lock<std::mutex> lk(p_->mu);
    if(p_->queue.empty()) break;
    Entry e = p_->queue.front(); p_->queue.pop_front();
    lk.unlock();
    for(auto& s : p_->subs){
      if(!match(s.pattern, e.key)) continue;
      double last = s.last.count(e.key) ? s.last[e.key] : -1e18;
      if(s.min_interval>0 && e.ts-last < s.min_interval) continue;
      s.last[e.key]=e.ts;
      s.h(e);
    }
  }
}
bool Blackboard::run_until(const std::function<bool()>& done, double timeout_s) {
  const double deadline = now() + timeout_s;
  while (!done()) {
    pump();
    if (done()) return true;
    std::unique_lock<std::mutex> lk(p_->mu);
    if (p_->queue.empty())
      p_->cv.wait_for(lk, std::chrono::milliseconds(20), [&]{ return !p_->queue.empty(); });
    lk.unlock();
    if (now() >= deadline) return false;
  }
  return true;
}
}  // namespace hades
