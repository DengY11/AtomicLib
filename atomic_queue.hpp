#ifndef ATOMIC_QUEUE_HPP
#define ATOMIC_QUEUE_HPP
#include <atomic>
#include <memory>
#include <optional>

template <typename EleType>
class AtomicQueue{
private:
    struct Node;
    using NodePtr = std::shared_ptr<Node>;

public:
    AtomicQueue()
        : head_(std::make_shared<Node>()),
        tail_(head_.load(std::memory_order_relaxed)){}
    ~AtomicQueue()=default;
    AtomicQueue(const AtomicQueue&)=delete;
    AtomicQueue& operator=(const AtomicQueue&)=delete;
    AtomicQueue(AtomicQueue&&)=delete;
    AtomicQueue& operator=(AtomicQueue&&)=delete;

    void enqueue(const EleType& ele){
        NodePtr new_node = std::make_shared<Node>(ele);
        enqueue_impl(std::move(new_node));
        
    }

    void enqueue(EleType&& ele){
        NodePtr new_node = std::make_shared<Node>(std::move(ele));
        enqueue_impl(std::move(new_node));
    }

private:
    void enqueue_impl(NodePtr new_node){
         for(;;){
            NodePtr tail = tail_.load(std::memory_order_acquire);
            NodePtr next = tail->next_.load(std::memory_order_acquire);
            if(!next){
                if(tail->next_.compare_exchange_weak(
                        next, new_node,
                        std::memory_order_release,
                        std::memory_order_relaxed)){
                    tail_.compare_exchange_weak(
                        tail, new_node,
                        std::memory_order_release,
                        std::memory_order_relaxed);
                    return;
                }
            }else{
                tail_.compare_exchange_weak(
                    tail, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
            }
        }
    }

public:
    [[nodiscard]] bool try_dequeue(EleType& out){
        for(;;){
            NodePtr head = head_.load(std::memory_order_acquire);
            NodePtr tail = tail_.load(std::memory_order_acquire);
            NodePtr next = head->next_.load(std::memory_order_acquire);
            if(!next){
                return false;
            }
            if(head == tail){
                tail_.compare_exchange_weak(
                    tail, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
                continue;
            }
            if(head_.compare_exchange_weak(
                    head, next,
                    std::memory_order_release,
                    std::memory_order_relaxed)){
                out = std::move(*(next->value_));
                return true;
            }
        }
    }

private:
    struct Node{
        Node()=default;
        explicit Node(const EleType& v): value_(v){}
        explicit Node(EleType&& v): value_(std::move(v)){}
        std::optional<EleType> value_;
        std::atomic<NodePtr> next_{nullptr};
    };
    std::atomic<NodePtr> head_;
    std::atomic<NodePtr> tail_;
};

#endif
