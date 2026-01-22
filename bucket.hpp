#ifndef BUCKET_HPP
#define BUCKET_HPP
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
namespace atomic{

class Bucket{
public:
    Bucket(int time_mms, double cap, double speed)
		: stop_(false),
		time_mms_(time_mms),
		cap_(cap),
		speed_(speed),
		current_(0.0){
		add_threads.emplace_back([this]()->void{
			add_spin();
		});
	}
    ~Bucket(){
		stop();
	}
	Bucket(const Bucket&)=delete;
	Bucket& operator=(const Bucket&)=delete;
	Bucket(Bucket&&)=delete;
	Bucket& operator=(Bucket&&)=delete;

	[[nodiscard]] double load(std::memory_order order = std::memory_order_relaxed) const{
		return current_.load(order);
	}

	[[nodiscard]] double capacity() const{
		return cap_;
	}

	[[nodiscard]]bool consume(double n){
		if(n <= 0.0){
			return false;
		}
		double cur = current_.load(std::memory_order_relaxed);
		while(cur >=n ){
			if(current_.compare_exchange_weak(cur, cur - n,
				std::memory_order_relaxed,
				std::memory_order_relaxed)){
				return true;
			}
		}
		return false;
	}
	bool stop(){
		bool expected = false;
		if(!stop_.compare_exchange_strong(expected, true, std::memory_order_relaxed)){
			return false;
		}
		for(auto& t: add_threads){
			if(t.joinable()){
				t.join();
			}
		}
		return true;
	}
private:
    void add_spin(){
		const double add_once = speed_ * time_mms_ / 1000.0;
		for(;!stop_.load(std::memory_order_relaxed);){
			double cur = current_.load(std::memory_order_relaxed);
			while(cur<cap_){
				double next = cur + add_once;
				if(next > cap_){
					next = cap_;
				}
				if(current_.compare_exchange_weak(cur, next,
					std::memory_order_relaxed,
					std::memory_order_relaxed)){
					break;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(time_mms_));
		}
    }

	std::atomic<bool>stop_;
	int time_mms_;
    double cap_;
    double speed_;
    std::atomic<double> current_;
	std::vector<std::thread>add_threads;
};

}


#endif
