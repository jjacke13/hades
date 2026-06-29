#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include "hades/executor.h"
using namespace hades;

TEST(Executor, RunsSubmittedTaskOffThread) {
  std::atomic<int> n{0};
  std::thread::id caller = std::this_thread::get_id();
  std::atomic<std::thread::id> ran_on{};
  { Executor ex(2);
    ex.submit([&]{ ran_on = std::this_thread::get_id(); n.fetch_add(1); });
    // dtor joins -> task definitely completed
  }
  EXPECT_EQ(n.load(), 1);
  EXPECT_NE(ran_on.load(), caller);   // ran on a worker, not the caller
}

TEST(Executor, RunsManyTasks) {
  std::atomic<int> n{0};
  { Executor ex(4);
    for (int i = 0; i < 100; ++i) ex.submit([&]{ n.fetch_add(1); });
  }
  EXPECT_EQ(n.load(), 100);
}

TEST(Executor, ThrowingTaskDoesNotCrash) {
  std::atomic<int> n{0};
  { Executor ex(2);
    ex.submit([]{ throw std::runtime_error("boom"); });
    ex.submit([&]{ n.fetch_add(1); });
  }
  EXPECT_EQ(n.load(), 1);   // the throwing task was contained; the other still ran
}
