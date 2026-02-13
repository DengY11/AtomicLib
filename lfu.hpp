#pragma once

#include <cstddef>
#include <map>
#include <list>
#include <memory>
#include <optional>
#include <algorithm>
#include <unordered_map>
#include <mutex>
#include <utility>
namespace atomic{

template <typename KeyTp, typename ValTp>
class LFU{
public:
    struct LFU_KV{
        KeyTp key_;
        std::shared_ptr<ValTp> val_;
        LFU_KV()=default;
        LFU_KV(const KeyTp& key, const ValTp& val)
            : key_(key),
              val_(std::make_shared<ValTp>(val)) {}
        LFU_KV(KeyTp&& key, ValTp&& val)
            : key_(std::move(key)),
              val_(std::make_shared<ValTp>(std::move(val))) {}
        LFU_KV(const KeyTp& key, std::shared_ptr<ValTp> val)
            : key_(key),
              val_(std::move(val)) {}
        LFU_KV(KeyTp&& key, std::shared_ptr<ValTp> val)
            : key_(std::move(key)),
              val_(std::move(val)) {}
    };
    explicit LFU(std::size_t cap)
        : min_freq_(0),
          cap_(cap),
          cur_cnt_(0) {}
    ~LFU()=default;
    LFU(const LFU&)=delete;
    LFU& operator=(const LFU&)=delete;
    LFU(LFU&&)=delete;
    LFU& operator=(LFU&&)=delete;

    [[nodiscard]] std::shared_ptr<ValTp> get(const KeyTp& key) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = key_to_freq_.find(key);
        if(it == key_to_freq_.end()){
            return nullptr;
        }
        std::size_t freq = it->second;
        auto list_it = freq_to_list_.find(freq);
        if(list_it == freq_to_list_.end()){
            return nullptr;
        }
        auto node_it_it = key_to_iter_.find(key);
        if(node_it_it == key_to_iter_.end()){
            return nullptr;
        }
        auto node_it = node_it_it->second;
        std::shared_ptr<ValTp> out = node_it->val_;

        auto& from = list_it->second;
        auto& to = freq_to_list_[freq + 1];
        to.splice(to.end(), from, node_it);
        key_to_iter_[key] = std::prev(to.end());
        it->second = freq + 1;

        if(from.empty()){
            freq_to_list_.erase(list_it);
            if(min_freq_ == freq){
                ++min_freq_;
            }
        }
        return out;
    }
    bool get(const KeyTp& key, ValTp& out){
        auto ptr = get(key);
        if(!ptr){
            return false;
        }
        out = *ptr;
        return true;
    }

    [[nodiscard]] std::optional<ValTp> get_copy(const KeyTp& key){
        auto ptr = get(key);
        if(!ptr){
            return std::nullopt;
        }
        return *ptr;
    }

    struct LockedValue{
        std::unique_lock<std::mutex> lock_;
        std::shared_ptr<ValTp> ptr_;
        explicit operator bool() const noexcept { return ptr_ != nullptr; }
        ValTp& value() const { return *ptr_; }
    };

    LockedValue get_locked(const KeyTp& key){
        std::unique_lock<std::mutex> lock(mu_);
        auto it = key_to_freq_.find(key);
        if(it == key_to_freq_.end()){
            return LockedValue{std::move(lock), nullptr};
        }
        std::size_t freq = it->second;
        auto list_it = freq_to_list_.find(freq);
        if(list_it == freq_to_list_.end()){
            return LockedValue{std::move(lock), nullptr};
        }
        auto node_it_it = key_to_iter_.find(key);
        if(node_it_it == key_to_iter_.end()){
            return LockedValue{std::move(lock), nullptr};
        }
        auto node_it = node_it_it->second;

        auto& from = list_it->second;
        auto& to = freq_to_list_[freq + 1];
        to.splice(to.end(), from, node_it);
        key_to_iter_[key] = std::prev(to.end());
        it->second = freq + 1;

        if(from.empty()){
            freq_to_list_.erase(list_it);
            if(min_freq_ == freq){
                ++min_freq_;
            }
        }
        return LockedValue{std::move(lock), node_it->val_};
    }
    void put(const KeyTp& key, const ValTp& val){
        put_impl(key, std::make_shared<ValTp>(val));
    }
    void put(KeyTp&& key, ValTp&& val){
        put_impl(std::move(key), std::make_shared<ValTp>(std::move(val)));
    }
    void put(const KeyTp& key, std::shared_ptr<ValTp> val){
        put_impl(key, std::move(val));
    }
    void put(KeyTp&& key, std::shared_ptr<ValTp> val){
        put_impl(std::move(key), std::move(val));
    }
    void put(std::unique_ptr<LFU_KV> kv){
        if(!kv){
            return;
        }
        put_impl(std::move(kv->key_), std::move(kv->val_));
    }


private:
    template <typename K>
    void put_impl(K&& key, std::shared_ptr<ValTp> val){
        std::lock_guard<std::mutex> lock(mu_);
        if(cap_ == 0){
            return;
        }
        if(!val){
            return;
        }
        auto it = key_to_freq_.find(key);
        if(it != key_to_freq_.end()){
            std::size_t freq = it->second;
            auto list_it = freq_to_list_.find(freq);
            if(list_it != freq_to_list_.end()){
                auto node_it_it = key_to_iter_.find(key);
                if(node_it_it == key_to_iter_.end()){
                    return;
                }
                auto node_it = node_it_it->second;
                node_it->val_ = std::move(val);
                auto& from = list_it->second;
                auto& to = freq_to_list_[freq + 1];
                to.splice(to.end(), from, node_it);
                key_to_iter_[key] = std::prev(to.end());
                it->second = freq + 1;
                if(from.empty()){
                    freq_to_list_.erase(list_it);
                    if(min_freq_ == freq){
                        ++min_freq_;
                    }
                }
            }
            return;
        }

        if(cur_cnt_ >= cap_){
            auto min_it = freq_to_list_.find(min_freq_);
            if(min_it != freq_to_list_.end()){
                auto& lst = min_it->second;
                auto victim_it = lst.begin();
                if(victim_it != lst.end()){
                    key_to_iter_.erase(victim_it->key_);
                    key_to_freq_.erase(victim_it->key_);
                    lst.erase(victim_it);
                    --cur_cnt_;
                }
                if(lst.empty()){
                    freq_to_list_.erase(min_it);
                }
            }
        }

        auto& lst = freq_to_list_[1];
        lst.emplace_back(std::forward<K>(key), std::move(val));
        auto node_it = std::prev(lst.end());
        key_to_iter_[node_it->key_] = node_it;
        key_to_freq_[node_it->key_] = 1;
        min_freq_ = 1;
        ++cur_cnt_;
    }

    std::map<KeyTp, std::size_t>key_to_freq_;
    using List = std::list<LFU_KV>;
    std::unordered_map<std::size_t, List>freq_to_list_;
    std::map<KeyTp, typename List::iterator>key_to_iter_;
    std::size_t min_freq_;
    std::size_t cap_;
    std::size_t cur_cnt_;
    std::mutex mu_;


};

}
