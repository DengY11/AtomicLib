#ifndef RATE_LIMITER_COUNTER_HPP
#define RATE_LIMITER_COUNTER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
namespace atomic{

class RateLimiterCounter{
public:
    RateLimiterCounter(int64_t window_ms, int limit)
      :count_(0), window_start_ms_(0), window_ms_(window_ms), limit_(limit){}
    ~RateLimiterCounter()=default;
    RateLimiterCounter(const RateLimiterCounter&)=delete;
    RateLimiterCounter& operator=(const RateLimiterCounter&)=delete;
    RateLimiterCounter(RateLimiterCounter&&)=delete;
    RateLimiterCounter& operator=(RateLimiterCounter&&)=delete;

    [[nodiscard]]bool allow(std::memory_order success = std::memory_order_relaxed,
               std::memory_order failure = std::memory_order_relaxed){
        for(;;){
            int64_t now = now_ms();
            int64_t window_start = window_start_ms_.load(failure);
            if(now - window_start >= window_ms_){
                if(window_start_ms_.compare_exchange_weak(window_start, now, success, failure)){
                    count_.store(1, success);
                    return true;
                }
                now = now_ms();
                continue;
            }else{
                int count = count_.load(failure);
                if(count>=limit_){
                    if(window_start_ms_.load(failure) == window_start){
                        return false;
                    }
                    continue;
                }
                for(;;){
                    if(count >= limit_){
                        break;
                    }
                    if(count_.compare_exchange_weak(count, count + 1, success, failure)){
                        return true;
                    }
                }
            }
        }
    }

private:
    static int64_t now_ms(){
        using clock = std::chrono::steady_clock;
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now().time_since_epoch()).count();
    }
    std::atomic<int> count_;
    std::atomic<int64_t> window_start_ms_;
    int64_t window_ms_;
    int limit_;

};
}


#endif
