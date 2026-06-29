// src/core/executor.cpp — Executor worker pool implementation
//
// A mutex + condition_variable task queue drained by N worker threads. Each
// worker loops: wait until (stop || queue non-empty) -> if stop and queue
// empty, exit -> else pop one task and run it inside try/catch. submit() locks,
// pushes, notify_one.
//
// Teardown lives in Impl's OWN destructor (set stop under the SAME mutex the
// workers wait on, notify_all, then join every worker) — stop-under-lock-
// before-notify is what makes the wait predicate re-check and the join
// deadlock-free. Putting it in ~Impl (not ~Executor) makes it exception-safe:
// if a std::thread spawn throws partway through the ctor's worker loop, the
// already-fully-constructed p_ unique_ptr is destroyed during stack unwinding,
// which runs ~Impl and cleanly stops+joins whatever workers exist. ~Executor is
// therefore =default — the single owner of the join is ~Impl, so no joinable
// thread can ever reach the workers-vector destructor (which would terminate).

#include "hades/executor.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace hades {

struct Executor::Impl {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::function<void()>> queue;  // guarded by mu
  bool stop = false;                        // guarded by mu
  std::vector<std::thread> workers;

  void worker_loop() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [this] { return stop || !queue.empty(); });
        if (stop && queue.empty()) return;  // drained + asked to stop
        task = std::move(queue.front());
        queue.pop_front();
      }
      // Contain any throw: an unhandled exception escaping a worker thread would
      // call std::terminate and take the whole process down.
      try {
        task();
      } catch (...) {
      }
    }
  }

  // Self-contained teardown: stop under the wait mutex, wake every worker to
  // re-check the predicate, then join all of them. Runs on BOTH the normal path
  // (~Executor -> member p_ destroyed -> ~Impl) and the ctor-throw path (p_
  // destroyed during stack unwinding). Joining here, in the destructor body
  // before the workers vector is destroyed, guarantees no joinable thread ever
  // reaches the vector dtor. This is the SOLE owner of the join.
  ~Impl() {
    {
      std::lock_guard<std::mutex> lk(mu);  // set stop under the wait mutex...
      stop = true;
    }
    cv.notify_all();  // ...then wake every worker to re-check the predicate
    for (auto& w : workers)
      if (w.joinable()) w.join();
  }
};

Executor::Executor(unsigned threads) : p_(std::make_unique<Impl>()) {
  const unsigned n = threads > 0 ? threads : 1u;  // at least one worker
  p_->workers.reserve(n);
  // Workers capture the Impl pointer directly (not the Executor `this`): if a
  // spawn throws mid-loop, p_ is destroyed during unwinding and a worker must
  // never race on a half-destroyed Executor member. Impl outlives every join.
  Impl* impl = p_.get();
  for (unsigned i = 0; i < n; ++i)
    p_->workers.emplace_back([impl] { impl->worker_loop(); });
}

// Teardown is owned by ~Impl (runs when the p_ unique_ptr member is destroyed),
// which keeps stop+notify+join exception-safe on the ctor-throw path too.
Executor::~Executor() = default;

void Executor::submit(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->stop) return;  // shutting down: drop rather than enqueue-never-run
    p_->queue.push_back(std::move(task));
  }
  p_->cv.notify_one();
}

}  // namespace hades
