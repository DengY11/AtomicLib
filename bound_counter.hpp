#ifndef BOUND_COUNTER_HPP
#define BOUND_COUNTER_HPP
#include <atomic>
#include <type_traits>
namespace atomic{

template<typename T, typename std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
class BoundCounter{
public:
    explicit BoundCounter(T cap):cap_(cap), current_(T{}){}
    ~BoundCounter()=default;

    BoundCounter(const BoundCounter&)=delete;
    BoundCounter& operator=(const BoundCounter&)=delete;
    BoundCounter(BoundCounter&&)=delete;
    BoundCounter& operator=(BoundCounter&&)=delete;

    [[nodiscard]] auto load(std::memory_order order = std::memory_order_relaxed) const -> T{
        return current_.load(order);
    }

    [[nodiscard]] auto capacity() const -> T{
        return cap_;
    }

    [[nodiscard]] auto try_add(T val)->bool{
        if constexpr (std::is_signed_v<T>){
            if(val < T{}){
                return false;
            }
        }
        if(val > cap_){
            return false;
        }
        T cur = current_.load(std::memory_order_relaxed);
        for(;;){
            if(cur > cap_ - val){
                return false;
            }
            const T next = cur + val;
            if(current_.compare_exchange_weak(cur, next,
                std::memory_order_relaxed,
                std::memory_order_relaxed)){
                return true;
            }
        }
    }
    [[nodiscard]] auto try_sub(T val)->bool{
        if constexpr (std::is_signed_v<T>){
            if(val < T{}){
                return false;
            }
        }
        T cur = current_.load(std::memory_order_relaxed);
        for(;;){
            if(cur < val){
                return false;
            }
            const T next = cur - val;
            if(current_.compare_exchange_weak(cur, next,
                std::memory_order_relaxed,
                std::memory_order_relaxed)){
                return true;
            }
        }
    }

private:
    T cap_;
    std::atomic<T>current_;
};
}

#endif
