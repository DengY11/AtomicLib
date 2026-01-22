#include "atomic_ring.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <pthread.h>
#include <sched.h>
#include <thread>
#include <vector>

constexpr std::size_t kCap = 1 << 30;

struct MutexQueue {
  bool try_enqueue(int v) {
    std::lock_guard<std::mutex> lock(mu_);
    q_.push(v);
    return true;
  }

  bool try_dequeue(int& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (q_.empty()) {
      return false;
    }
    out = q_.front();
    q_.pop();
    return true;
  }

  std::mutex mu_;
  std::queue<int> q_;
};

using RingQueue = atomic::MPMC::RingBuffer<int, kCap>;

struct BenchResult {
  const char* name;
  int64_t produced;
  int64_t consumed;
  double seconds;
};

static void pin_thread(std::size_t cpu) {
#ifdef __linux__
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<int>(cpu), &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
  (void)cpu;
#endif
}

template <typename Queue>
static BenchResult run_bench(const char* name, int producers, int consumers, int seconds) {
  auto q = std::make_unique<Queue>();
  std::atomic<bool> start{false};
  std::atomic<int> phase{0};
  const std::size_t cpu_count = std::max(1u, std::thread::hardware_concurrency());
  std::vector<int64_t> produced_counts(producers, 0);
  std::vector<int64_t> consumed_counts(consumers, 0);

  std::vector<std::thread> threads;
  threads.reserve(producers + consumers);

  for (int i = 0; i < producers; ++i) {
    threads.emplace_back([&, i]() {
      if (cpu_count > 0) {
        pin_thread(static_cast<std::size_t>(i) % cpu_count);
      }
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      int v = 0;
      int64_t local_count = 0;
      for (;;) {
        const int cur_phase = phase.load(std::memory_order_relaxed);
        if (cur_phase == 2) {
          break;
        }
        if (q->try_enqueue(v++)) {
          if (cur_phase == 1) {
            ++local_count;
          }
        } else {
          std::this_thread::yield();
        }
      }
      produced_counts[i] = local_count;
    });
  }

  for (int i = 0; i < consumers; ++i) {
    threads.emplace_back([&, i]() {
      if (cpu_count > 0) {
        pin_thread(static_cast<std::size_t>(producers + i) % cpu_count);
      }
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      int out = 0;
      int64_t local_count = 0;
      for (;;) {
        const int cur_phase = phase.load(std::memory_order_relaxed);
        if (cur_phase == 2) {
          break;
        }
        if (q->try_dequeue(out)) {
          if (cur_phase == 1) {
            ++local_count;
          }
        } else {
          std::this_thread::yield();
        }
      }
      consumed_counts[i] = local_count;
    });
  }

  start.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  const auto t0 = std::chrono::steady_clock::now();
  phase.store(1, std::memory_order_relaxed);
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  phase.store(2, std::memory_order_relaxed);
  const auto t1 = std::chrono::steady_clock::now();

  for (auto& t : threads) {
    t.join();
  }

  int64_t produced = 0;
  int64_t consumed = 0;
  for (int64_t v : produced_counts) {
    produced += v;
  }
  for (int64_t v : consumed_counts) {
    consumed += v;
  }
  const std::chrono::duration<double> elapsed = t1 - t0;
  return BenchResult{name, produced, consumed, elapsed.count()};
}

static void print_result(const BenchResult& r) {
  const double ops = r.consumed / r.seconds;
  std::cout << r.name << ": produced=" << r.produced
            << " consumed=" << r.consumed
            << " seconds=" << r.seconds
            << " ops/s=" << ops << "\n";
}

int main(int argc, char** argv) {
  int producers = 4;
  int consumers = 4;
  int seconds = 2;

  if (argc >= 2) producers = std::atoi(argv[1]);
  if (argc >= 3) consumers = std::atoi(argv[2]);
  if (argc >= 4) seconds = std::atoi(argv[3]);

  auto r1 = run_bench<RingQueue>("RingQueue", producers, consumers, seconds);
  auto r2 = run_bench<MutexQueue>("MutexQueue", producers, consumers, seconds);

  print_result(r1);
  print_result(r2);

  return 0;
}
