#ifndef ATOMIC_QUEUE_HPP
#define ATOMIC_QUEUE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

template <typename T>
class AtomicQueue {
public:
  AtomicQueue()
      : head_(new Node()),
        tail_(head_.load(std::memory_order_relaxed)) {}

  ~AtomicQueue() {
    Node* node = head_.load(std::memory_order_relaxed);
    while (node) {
      Node* next = node->next.load(std::memory_order_relaxed);
      delete node;
      node = next;
    }
    drain_free_list();
  }

  AtomicQueue(const AtomicQueue&) = delete;
  AtomicQueue& operator=(const AtomicQueue&) = delete;
  AtomicQueue(AtomicQueue&&) = delete;
  AtomicQueue& operator=(AtomicQueue&&) = delete;

  void enqueue(const T& value) {
    Node* node = make_node(value);
    enqueue_impl(node);
  }

  void enqueue(T&& value) {
    Node* node = make_node(std::move(value));
    enqueue_impl(node);
  }

  [[nodiscard]] bool try_dequeue(T& out) {
    EpochGuard guard(epoch_);
    for (;;) {
      Node* head = head_.load(std::memory_order_acquire);
      Node* tail = tail_.load(std::memory_order_acquire);
      Node* next = head->next.load(std::memory_order_acquire);
      if (!next) {
        return false;
      }
      if (head == tail) {
        tail_.compare_exchange_weak(
            tail, next,
            std::memory_order_release,
            std::memory_order_relaxed);
        continue;
      }
      if (head_.compare_exchange_weak(
              head, next,
              std::memory_order_release,
              std::memory_order_relaxed)) {
        out = std::move(*(next->value));
        epoch_.retire(head);
        return true;
      }
    }
  }

private:
  static constexpr std::size_t kCacheLine = 64;
  static constexpr std::size_t kRetireThreshold = 64;
  static constexpr std::size_t kLocalCacheLimit = 64;

  struct Node {
    Node() = default;
    explicit Node(const T& v) : value(v) {}
    explicit Node(T&& v) : value(std::move(v)) {}
    std::optional<T> value;
    std::atomic<Node*> next{nullptr};
  };

  struct Retired {
    Node* node;
    uint64_t epoch;
  };

  struct alignas(kCacheLine) ThreadRecord {
    std::atomic<uint64_t> epoch{0};
    std::atomic<bool> active{false};
    ThreadRecord* next{nullptr};
    std::vector<Retired> retired;
    Node* local_free{nullptr};
    std::size_t local_count{0};
  };

  class EpochManager {
  public:
    explicit EpochManager(AtomicQueue* owner)
        : owner_(owner) {}
    ~EpochManager() {
      ThreadRecord* node = records_.load(std::memory_order_relaxed);
      while (node) {
        for (const auto& retired : node->retired) {
          delete retired.node;
        }
        node->retired.clear();
        owner_->drain_local_cache(node);
        ThreadRecord* next = node->next;
        delete node;
        node = next;
      }
    }

    ThreadRecord* get_record() {
      ThreadRecord* record = find_record();
      if (record) {
        return record;
      }
      record = new ThreadRecord();
      ThreadRecord* head = records_.load(std::memory_order_acquire);
      do {
        record->next = head;
      } while (!records_.compare_exchange_weak(
          head, record,
          std::memory_order_release,
          std::memory_order_relaxed));
      register_record(record);
      return record;
    }

    void retire(Node* node) {
      ThreadRecord* record = get_record();
      record->retired.push_back(Retired{node, global_epoch_.load(std::memory_order_relaxed)});
      if (record->retired.size() >= kRetireThreshold) {
        scan(record);
      }
    }

    class Guard {
    public:
      explicit Guard(EpochManager& manager)
          : manager_(manager),
            record_(manager_.get_record()) {
        record_->epoch.store(manager_.global_epoch_.load(std::memory_order_acquire),
                             std::memory_order_release);
        record_->active.store(true, std::memory_order_release);
      }

      ~Guard() {
        record_->active.store(false, std::memory_order_release);
      }

    private:
      EpochManager& manager_;
      ThreadRecord* record_;
    };

  private:
    void scan(ThreadRecord* record) {
      advance_epoch();
      const uint64_t cur = global_epoch_.load(std::memory_order_acquire);
      const uint64_t safe_epoch = (cur >= 2) ? cur - 2 : 0;

      std::vector<Retired> remaining;
      remaining.reserve(record->retired.size());
      for (const auto& r : record->retired) {
        if (r.epoch <= safe_epoch) {
          owner_->reclaim_node(r.node);
        } else {
          remaining.push_back(r);
        }
      }
      record->retired.swap(remaining);
    }

    void advance_epoch() {
      const uint64_t cur = global_epoch_.load(std::memory_order_acquire);
      ThreadRecord* node = records_.load(std::memory_order_acquire);
      while (node) {
        if (node->active.load(std::memory_order_acquire) &&
            node->epoch.load(std::memory_order_acquire) != cur) {
          return;
        }
        node = node->next;
      }
      uint64_t expected = cur;
      global_epoch_.compare_exchange_weak(
          expected, cur + 1,
          std::memory_order_release,
          std::memory_order_relaxed);
    }

    ThreadRecord* find_record() {
      auto& slots = tls_records();
      for (auto& slot : slots) {
        if (slot.manager == this) {
          return slot.record;
        }
      }
      return nullptr;
    }

    void register_record(ThreadRecord* record) {
      tls_records().push_back(Slot{this, record});
    }

    struct Slot {
      EpochManager* manager;
      ThreadRecord* record;
    };

    static std::vector<Slot>& tls_records() {
      thread_local std::vector<Slot> records;
      return records;
    }

    alignas(kCacheLine) std::atomic<uint64_t> global_epoch_{0};
    std::atomic<ThreadRecord*> records_{nullptr};
    AtomicQueue* owner_{nullptr};
  };

  void enqueue_impl(Node* node) {
    EpochGuard guard(epoch_);
    for (;;) {
      Node* tail = tail_.load(std::memory_order_acquire);
      Node* next = tail->next.load(std::memory_order_acquire);
      if (!next) {
        if (tail->next.compare_exchange_weak(
                next, node,
                std::memory_order_release,
                std::memory_order_relaxed)) {
          tail_.compare_exchange_weak(
              tail, node,
              std::memory_order_release,
              std::memory_order_relaxed);
          return;
        }
      } else {
        tail_.compare_exchange_weak(
            tail, next,
            std::memory_order_release,
            std::memory_order_relaxed);
      }
    }
  }

  using EpochGuard = typename EpochManager::Guard;

  Node* make_node(const T& value) {
    Node* node = acquire_node();
    node->value.emplace(value);
    return node;
  }

  Node* make_node(T&& value) {
    Node* node = acquire_node();
    node->value.emplace(std::move(value));
    return node;
  }

  Node* acquire_node() {
    ThreadRecord* record = epoch_.get_record();
    if (record->local_free) {
      Node* node = record->local_free;
      record->local_free = node->next.load(std::memory_order_relaxed);
      record->local_count--;
      node->next.store(nullptr, std::memory_order_relaxed);
      return node;
    }
    Node* node = pop_global();
    if (node) {
      node->next.store(nullptr, std::memory_order_relaxed);
      return node;
    }
    return new Node();
  }

  void reclaim_node(Node* node) {
    node->value.reset();
    ThreadRecord* record = epoch_.get_record();
    node->next.store(record->local_free, std::memory_order_relaxed);
    record->local_free = node;
    record->local_count++;
    if (record->local_count >= kLocalCacheLimit) {
      flush_local_cache(record);
    }
  }

  void flush_local_cache(ThreadRecord* record) {
    while (record->local_free && record->local_count > kLocalCacheLimit / 2) {
      Node* node = record->local_free;
      record->local_free = node->next.load(std::memory_order_relaxed);
      record->local_count--;
      push_global(node);
    }
  }

  void drain_local_cache(ThreadRecord* record) {
    Node* node = record->local_free;
    while (node) {
      Node* next = node->next.load(std::memory_order_relaxed);
      delete node;
      node = next;
    }
    record->local_free = nullptr;
    record->local_count = 0;
  }

  Node* pop_global() {
    Node* head = free_head_.load(std::memory_order_acquire);
    while (head) {
      Node* next = head->next.load(std::memory_order_relaxed);
      if (free_head_.compare_exchange_weak(
              head, next,
              std::memory_order_acquire,
              std::memory_order_relaxed)) {
        return head;
      }
    }
    return nullptr;
  }

  void push_global(Node* node) {
    Node* head = free_head_.load(std::memory_order_relaxed);
    do {
      node->next.store(head, std::memory_order_relaxed);
    } while (!free_head_.compare_exchange_weak(
        head, node,
        std::memory_order_release,
        std::memory_order_relaxed));
  }

  void drain_free_list() {
    Node* node = free_head_.exchange(nullptr, std::memory_order_acq_rel);
    while (node) {
      Node* next = node->next.load(std::memory_order_relaxed);
      delete node;
      node = next;
    }
  }

  EpochManager epoch_{this};
  alignas(kCacheLine) std::atomic<Node*> head_;
  alignas(kCacheLine) std::atomic<Node*> tail_;
  alignas(kCacheLine) std::atomic<Node*> free_head_{nullptr};
};

#endif
