#ifndef ATOMIC_RING_HPP
#define ATOMIC_RING_HPP
#include <array>
#include <atomic>
#include <cstddef>
#include <utility>

namespace atomic{
namespace MPMC{

template <typename EleType, std::size_t Cap>
class RingBuffer{
private:
    struct Slot;
    static constexpr std::size_t kMask = Cap - 1;
    
public:
    static_assert((Cap & (Cap - 1)) == 0, "Cap must be power of two.");
    RingBuffer() : head_(0), tail_(0) {
        for(std::size_t i = 0; i < Cap; ++i){
            slots_[i].seq.store(i, std::memory_order_relaxed);
        }
    }
    ~RingBuffer()=default;
    RingBuffer(const RingBuffer&)=delete;
    RingBuffer& operator=(const RingBuffer&)=delete;
    RingBuffer(RingBuffer&&)=delete;
    RingBuffer& operator=(RingBuffer&&)=delete;

    bool try_enqueue(const EleType& ele){
        return enqueue_impl(ele);
    }
    bool try_enqueue(EleType&& ele){
        return enqueue_impl(std::move(ele));
    }
    bool enqueue_impl(EleType ele){
        std::size_t pos = tail_.load(std::memory_order_relaxed);
        for(;;){
            Slot& slot = slots_[pos & kMask];
            std::size_t seq = slot.seq.load(std::memory_order_acquire);
            std::ptrdiff_t diff = static_cast<std::ptrdiff_t>(seq) -
                static_cast<std::ptrdiff_t>(pos);
            if(diff == 0){
                if(tail_.compare_exchange_weak(
                        pos, pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)){
                    slot.ele_ = std::move(ele);
                    slot.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            }else if(diff < 0){
                return false;
            }else{
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
    }
    bool try_dequeue(EleType& out){
        std::size_t pos = head_.load(std::memory_order_relaxed);
        for(;;){
            Slot& slot = slots_[pos & kMask];
            std::size_t seq = slot.seq.load(std::memory_order_acquire);
            std::ptrdiff_t diff = static_cast<std::ptrdiff_t>(seq) -
                static_cast<std::ptrdiff_t>(pos + 1);
            if(diff == 0){
                if(head_.compare_exchange_weak(
                        pos, pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)){
                    out = std::move(slot.ele_);
                    slot.seq.store(pos + Cap, std::memory_order_release);
                    return true;
                }
            }else if(diff < 0){
                return false;
            }else{
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }



private:
    struct Slot{
        std::atomic<std::size_t> seq{0};
        EleType ele_;
    };
    alignas(64) std::atomic<std::size_t> head_;
    alignas(64) std::atomic<std::size_t> tail_;
    std::array<Slot, Cap> slots_;
};



}
}


#endif
