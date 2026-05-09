#include "utils/RateLimiter.hpp"

namespace nuigraph {

bool RateLimiter::allow(const std::string& key, std::size_t maxRequests, long long windowSeconds, long long nowSeconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& hits = hits_[key];
    while (!hits.empty() && nowSeconds - hits.front() >= windowSeconds) {
        hits.pop_front();
    }
    if (hits.size() >= maxRequests) {
        return false;
    }
    hits.push_back(nowSeconds);
    return true;
}

void RateLimiter::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    hits_.clear();
}

} // namespace nuigraph
