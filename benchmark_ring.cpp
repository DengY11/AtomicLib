#include "atomic_ring.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

constexpr std::size_t kCap = 1 << 16;

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

template <typename Queue>
static BenchResult run_bench(const char* name, int producers, int consumers, int seconds) {
  Queue q;
  std::atomic<bool> start{false};
  std::atomic<bool> stop{false};
  std::atomic<int64_t> produced{0};
  std::atomic<int64_t> consumed{0};

  std::vector<std::thread> threads;
  threads.reserve(producers + consumers);

  for (int i = 0; i < producers; ++i) {
    threads.emplace_back([&]() {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      int v = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        if (q.try_enqueue(v++)) {
          produced.fetch_add(1, std::memory_order_relaxed);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  for (int i = 0; i < consumers; ++i) {
    threads.emplace_back([&]() {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      int out = 0;
      while (!stop.load(std::memory_order_relaxed) ||
             consumed.load(std::memory_order_relaxed) < produced.load(std::memory_order_relaxed)) {
        if (q.try_dequeue(out)) {
          consumed.fetch_add(1, std::memory_order_relaxed);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  const auto t0 = std::chrono::steady_clock::now();
  start.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  stop.store(true, std::memory_order_relaxed);

  for (auto& t : threads) {
    t.join();
  }
  const auto t1 = std::chrono::steady_clock::now();
  const std::chrono::duration<double> elapsed = t1 - t0;

  return BenchResult{name, produced.load(), consumed.load(), elapsed.count()};
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
