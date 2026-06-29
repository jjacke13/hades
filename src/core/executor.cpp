// src/core/executor.cpp — Executor worker pool implementation
//
// A mutex + condition_variable task queue drained by N worker threads. Each
// worker loops: wait until (stop || queue non-empty) -> if stop and queue
// empty, exit -> else pop one task and run it inside try/catch. submit() locks,
// pushes, notify_one. The dtor sets stop under the SAME mutex the workers wait
// on, notify_all, then joins every worker — stop-under-lock-before-notify is
// what makes the wait predicate re-check and the join deadlock-free.

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
};

Executor::Executor(unsigned threads) : p_(std::make_unique<Impl>()) {
  const unsigned n = threads > 0 ? threads : 1u;  // at least one worker
  p_->workers.reserve(n);
  for (unsigned i = 0; i < n; ++i)
    p_->workers.emplace_back([this] { p_->worker_loop(); });
}

Executor::~Executor() {
  {
    std::lock_guard<std::mutex> lk(p_->mu);  // set stop under the wait mutex...
    p_->stop = true;
  }
  p_->cv.notify_all();  // ...then wake every worker to re-check the predicate
  for (auto& w : p_->workers)
    if (w.joinable()) w.join();
}

void Executor::submit(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->stop) return;  // shutting down: drop rather than enqueue-never-run
    p_->queue.push_back(std::move(task));
  }
  p_->cv.notify_one();
}

}  // namespace hades
