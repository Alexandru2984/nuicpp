#include "utils/RateLimiter.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace nuigraph {

bool RateLimiter::allow(const std::string& key, std::size_t maxRequests, long long windowSeconds, long long nowSeconds) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (hits_.size() >= kMaxTrackedKeys) {
        evictLocked(windowSeconds, nowSeconds);
    }

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

void RateLimiter::evictLocked(long long windowSeconds, long long nowSeconds) {
    // Drop every key whose newest hit already fell out of the window; those
    // callers are indistinguishable from ones never seen before.
    for (auto it = hits_.begin(); it != hits_.end();) {
        if (it->second.empty() || nowSeconds - it->second.back() >= windowSeconds) {
            it = hits_.erase(it);
        } else {
            ++it;
        }
    }

    if (hits_.size() < kMaxTrackedKeys) {
        return;
    }

    // Everything is still inside the window, so something must be dropped.
    // Evicting a key resets its counter, which would otherwise hand an attacker
    // a way to clear their own throttle by flooding the map with fresh keys.
    // Rank by hit count first so the keys closest to their limit survive, and
    // use recency only to break ties; the entries discarded are the ones that
    // have barely been seen and lose the least by starting over.
    struct Ranked {
        std::size_t hits;
        long long lastSeen;
        const std::string* key;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(hits_.size());
    for (const auto& entry : hits_) {
        ranked.push_back({entry.second.size(), entry.second.back(), &entry.first});
    }
    const auto keep = static_cast<std::ptrdiff_t>(kMaxTrackedKeys / 2);
    std::nth_element(ranked.begin(), ranked.begin() + keep, ranked.end(), [](const Ranked& a, const Ranked& b) {
        if (a.hits != b.hits) return a.hits > b.hits;
        return a.lastSeen > b.lastSeen;
    });
    std::vector<std::string> doomed;
    doomed.reserve(ranked.size() - static_cast<std::size_t>(keep));
    for (auto it = ranked.begin() + keep; it != ranked.end(); ++it) {
        doomed.push_back(*it->key);
    }
    for (const auto& key : doomed) {
        hits_.erase(key);
    }
}

void RateLimiter::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    hits_.clear();
}

std::size_t RateLimiter::trackedKeys() {
    std::lock_guard<std::mutex> lock(mutex_);
    return hits_.size();
}

} // namespace nuigraph
