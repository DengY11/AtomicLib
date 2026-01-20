#ifndef ATOMIC_CLAMP_HPP
#define ATOMIC_CLAMP_HPP

#include <atomic>
#include <cassert>
#include <type_traits>

template <typename T, typename std::enable_if_t<std::is_arithmetic_v<T>,int> = 0>
class AtomicClamp{
public:
    explicit AtomicClamp(T init):atom_(init){}
    ~AtomicClamp()=default;
    AtomicClamp(const AtomicClamp&)=delete;
    AtomicClamp& operator=(const AtomicClamp&)=delete;
    AtomicClamp(AtomicClamp&&)=delete;
    AtomicClamp& operator=(AtomicClamp&&)=delete;

    T load(std::memory_order order = std::memory_order_relaxed) const{
        return atom_.load(order);

    }
    bool clamp_to(T low, T high,
        std::memory_order success = std::memory_order_relaxed,
        std::memory_order failure = std::memory_order_relaxed){
        assert(low <= high);
        T cur = atom_.load(failure);
        for(;;){
            if(cur < low){
                if(atom_.compare_exchange_weak(cur, low, success, failure)){
                    return true;
                }
            }else if(cur > high){
                if(atom_.compare_exchange_weak(cur, high, success, failure)){
                    return true;
                }
            }else{
                return false;
            }
        }
    }

private:
    std::atomic<T> atom_;
};

#endif
