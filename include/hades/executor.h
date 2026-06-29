// include/hades/executor.h — generic worker-thread pool
//
// Offloads blocking work (e.g. the LLM HTTP call) off the single-threaded bus.
// Submit a std::function<void()>; one of N worker threads runs it. The Executor
// knows NOTHING about the Blackboard — a submitted closure captures whatever it
// needs and calls bb.post(...) itself (post() is thread-safe). Each task runs
// inside a try/catch so an unhandled throw on a worker can't std::terminate the
// process. The dtor signals stop, wakes all workers, and joins them (queued
// tasks already enqueued are drained before a worker exits).

#pragma once
#include <functional>
#include <memory>

namespace hades {
class Executor {
public:
  explicit Executor(unsigned threads);  // spawn N worker threads (>=1)
  ~Executor();                           // signal stop, notify, join all workers
  Executor(const Executor&) = delete;
  Executor& operator=(const Executor&) = delete;
  Executor(Executor&&) = delete;
  Executor& operator=(Executor&&) = delete;
  // Enqueue a task for a worker thread. After the dtor has begun (stop set) the
  // task is dropped rather than enqueued. Any exception thrown by a submitted
  // task is caught and swallowed on the worker thread (it will not propagate or
  // kill the worker).
  void submit(std::function<void()> task);

private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};
}  // namespace hades
