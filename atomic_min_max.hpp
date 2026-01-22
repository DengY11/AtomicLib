#ifndef ATOMIC_MIN_MAX_HPP
#define ATOMIC_MIN_MAX_HPP

#include <atomic>
#include <cmath>
#include <type_traits>
namespace atomic{

template <typename T,
    typename std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
class MinMax{
public:
    explicit MinMax(T val):cur_(val){}
    ~MinMax()=default;

    MinMax(const MinMax&)=delete;
    MinMax& operator=(const MinMax&)=delete;
    MinMax(MinMax&&)=delete;
    MinMax& operator=(MinMax&&)=delete;

    T load(std::memory_order order = std::memory_order_relaxed) const{
        return cur_.load(order);
    }
    [[nodiscard]]bool update_min(
        T v,
        std::memory_order success = std::memory_order_relaxed,
        std::memory_order failure = std::memory_order_relaxed){
        if constexpr (std::is_floating_point_v<T>){
            if(std::isnan(v)){
                return false;
            }
        }
        T cur = cur_.load(failure);
        for(;;){
            if constexpr (std::is_floating_point_v<T>){
                if(std::isnan(cur)){
                    if(cur_.compare_exchange_weak(cur, v, success, failure)){
                        return true;
                    }
                    continue;
                }
            }
            if(cur <= v){
                return false;
            }
            if(cur_.compare_exchange_weak(cur, v,
                success, failure)){
                return true;
            }
        }
    }
    [[nodiscard]]bool update_max(
        T v,
        std::memory_order success = std::memory_order_relaxed,
        std::memory_order failure = std::memory_order_relaxed){
        if constexpr (std::is_floating_point_v<T>){
            if(std::isnan(v)){
                return false;
            }
        }
        T cur = cur_.load(failure);
        for(;;){
            if constexpr (std::is_floating_point_v<T>){
                if(std::isnan(cur)){
                    if(cur_.compare_exchange_weak(cur, v, success, failure)){
                        return true;
                    }
                    continue;
                }
            }
            if(cur >= v){
                return false;
            }
            if(cur_.compare_exchange_weak(cur, v,
                success, failure)){
                return true;
            }
        }
    }
private:
    std::atomic<T>cur_;


};
}

#endif
