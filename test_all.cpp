#include "atomic_clamp.hpp"
#include "atomic_min_max.hpp"
#include "atomic_queue.hpp"
#include "atomic_ring.hpp"
#include "bound_counter.hpp"
#include "bucket.hpp"
#include "lfu.hpp"
#include "rate_limiter_counter.hpp"

#include <cassert>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace atomic;

static void test_bound_counter() {
  BoundCounter<int> bc(5);
  assert(bc.load() == 0);
  assert(bc.try_add(3));
  assert(bc.load() == 3);
  assert(!bc.try_add(3));
  assert(bc.try_sub(2));
  assert(bc.load() == 1);
  assert(!bc.try_sub(5));
}

static void test_atomic_min_max() {
  MinMax<double> mm(10.0);
  assert(mm.load() == 10.0);
  assert(mm.update_min(5.0));
  assert(mm.load() == 5.0);
  assert(!mm.update_min(6.0));
  assert(mm.update_max(12.0));
  assert(mm.load() == 12.0);
  assert(!mm.update_max(11.0));
  assert(!mm.update_min(std::nan("")));
  assert(!mm.update_max(std::nan("")));
}

static void test_atomic_clamp() {
  Clamp<int> clamp(5);
  assert(!clamp.clamp_to(0, 10));
  assert(clamp.load() == 5);
  assert(clamp.clamp_to(6, 10));
  assert(clamp.load() == 6);
  assert(clamp.clamp_to(-5, 3));
  assert(clamp.load() == 3);
}

static void test_rate_limiter_counter() {
  RateLimiterCounter rl(50, 3);
  assert(rl.allow());
  assert(rl.allow());
  assert(rl.allow());
  assert(!rl.allow());
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  assert(rl.allow());
}

static void test_atomic_queue() {
  Queue<int> q;
  q.enqueue(1);
  q.enqueue(2);
  int out = 0;
  assert(q.try_dequeue(out));
  assert(out == 1);
  assert(q.try_dequeue(out));
  assert(out == 2);
  assert(!q.try_dequeue(out));
}

static void test_atomic_queue_concurrent() {
  constexpr int kProducers = 4;
  constexpr int kConsumers = 4;
  constexpr int kPerProducer = 20000;
  constexpr int kTotal = kProducers * kPerProducer;

  Queue<int> q;
  std::atomic<int> produced{0};
  std::atomic<int> consumed{0};
  std::atomic<long long> sum{0};

  std::vector<std::thread> producers;
  producers.reserve(kProducers);
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([p, &q, &produced]() {
      const int base = p * kPerProducer;
      for (int i = 0; i < kPerProducer; ++i) {
        q.enqueue(base + i);
        produced.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::vector<std::thread> consumers;
  consumers.reserve(kConsumers);
  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&]() {
      int value = 0;
      while (consumed.load(std::memory_order_relaxed) < kTotal) {
        if (q.try_dequeue(value)) {
          sum.fetch_add(value, std::memory_order_relaxed);
          consumed.fetch_add(1, std::memory_order_relaxed);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  for (auto& t : producers) {
    t.join();
  }
  for (auto& t : consumers) {
    t.join();
  }

  long long expected_sum = 0;
  for (int p = 0; p < kProducers; ++p) {
    const long long start = static_cast<long long>(p) * kPerProducer;
    const long long end = start + kPerProducer - 1;
    expected_sum += (start + end) * kPerProducer / 2;
  }

  assert(produced.load() == kTotal);
  assert(consumed.load() == kTotal);
  assert(sum.load() == expected_sum);
}

static void test_atomic_ring() {
  MPMC::RingBuffer<int, 8> q;
  int out = 0;
  assert(!q.try_dequeue(out));
  assert(q.try_enqueue(1));
  assert(q.try_enqueue(2));
  assert(q.try_dequeue(out));
  assert(out == 1);
  assert(q.try_dequeue(out));
  assert(out == 2);
  assert(!q.try_dequeue(out));
}

static void test_atomic_ring_concurrent() {
  constexpr int kProducers = 4;
  constexpr int kConsumers = 4;
  constexpr int kPerProducer = 20000;
  constexpr int kTotal = kProducers * kPerProducer;
  MPMC::RingBuffer<int, 1 << 16> q;

  std::atomic<int> produced{0};
  std::atomic<int> consumed{0};
  std::atomic<long long> sum{0};

  std::vector<std::thread> producers;
  producers.reserve(kProducers);
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([p, &q, &produced]() {
      const int base = p * kPerProducer;
      for (int i = 0; i < kPerProducer; ++i) {
        while (!q.try_enqueue(base + i)) {
          std::this_thread::yield();
        }
        produced.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::vector<std::thread> consumers;
  consumers.reserve(kConsumers);
  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&]() {
      int value = 0;
      while (consumed.load(std::memory_order_relaxed) < kTotal) {
        if (q.try_dequeue(value)) {
          sum.fetch_add(value, std::memory_order_relaxed);
          consumed.fetch_add(1, std::memory_order_relaxed);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  for (auto& t : producers) {
    t.join();
  }
  for (auto& t : consumers) {
    t.join();
  }

  long long expected_sum = 0;
  for (int p = 0; p < kProducers; ++p) {
    const long long start = static_cast<long long>(p) * kPerProducer;
    const long long end = start + kPerProducer - 1;
    expected_sum += (start + end) * kPerProducer / 2;
  }

  assert(produced.load() == kTotal);
  assert(consumed.load() == kTotal);
  assert(sum.load() == expected_sum);
}

static void test_bucket() {
  Bucket b(10, 5.0, 5.0);
  assert(!b.consume(1.0));
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  assert(b.consume(1.0));
}

static void test_bucket_concurrent() {
  constexpr int kThreads = 4;
  constexpr int kTotalTokens = 50;
  Bucket b(10, 50.0, 50.0);
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  b.stop();

  std::atomic<int> consumed{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&]() {
      while (consumed.load(std::memory_order_relaxed) < kTotalTokens) {
        if (b.consume(1.0)) {
          consumed.fetch_add(1, std::memory_order_relaxed);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  assert(consumed.load() == kTotalTokens);
}

static void test_lfu_eviction() {
  LFU<int, int> lfu(2);
  lfu.put(1, 10);
  lfu.put(2, 20);

  auto v1 = lfu.get(1);
  assert(v1 && *v1 == 10);

  lfu.put(3, 30);

  assert(!lfu.get(2));
  auto v1_after = lfu.get(1);
  assert(v1_after && *v1_after == 10);
  auto v3 = lfu.get(3);
  assert(v3 && *v3 == 30);
}

static void test_lfu_lru_within_freq() {
  LFU<int, int> lfu(2);
  lfu.put(1, 1);
  lfu.put(2, 2);

  lfu.put(3, 3);

  assert(!lfu.get(1));
  auto v2 = lfu.get(2);
  assert(v2 && *v2 == 2);
  auto v3 = lfu.get(3);
  assert(v3 && *v3 == 3);
}

static void test_lfu_update_existing() {
  LFU<int, int> lfu(2);
  lfu.put(1, 1);
  lfu.put(2, 2);

  lfu.put(1, 10);
  lfu.put(3, 3);

  assert(!lfu.get(2));
  auto v1 = lfu.get(1);
  assert(v1 && *v1 == 10);
}

static void test_lfu_accessors() {
  LFU<int, std::string> lfu(1);
  lfu.put(1, std::string("a"));

  auto v1 = lfu.get_copy(1);
  assert(v1 && *v1 == "a");

  {
    auto locked = lfu.get_locked(1);
    assert(locked);
    locked.value() = "b";
  }

  auto v2 = lfu.get_copy(1);
  assert(v2 && *v2 == "b");
}

static void test_lfu_put_kv() {
  LFU<int, int> lfu(1);
  auto kv = std::make_unique<LFU<int, int>::LFU_KV>(1, 11);
  lfu.put(std::move(kv));

  auto v1 = lfu.get(1);
  assert(v1 && *v1 == 11);
}

int main() {
  test_bound_counter();
  test_atomic_min_max();
  test_atomic_clamp();
  test_rate_limiter_counter();
  test_atomic_queue();
  test_atomic_queue_concurrent();
  test_atomic_ring();
  test_atomic_ring_concurrent();
  test_bucket();
  test_bucket_concurrent();
  test_lfu_eviction();
  test_lfu_lru_within_freq();
  test_lfu_update_existing();
  test_lfu_accessors();
  test_lfu_put_kv();

  std::cout << "PASS\n";

  return 0;
}
