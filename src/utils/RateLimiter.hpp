#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace nuigraph {

// Sliding-window counter keyed by caller-supplied strings (in practice
// "<bucket>:<client ip>").
//
// Every distinct key allocates an entry and keys come from the network, so the
// map is swept of expired entries once it grows past a threshold, then hard
// capped. Without that, traffic from changing addresses grows the map for the
// lifetime of the process.
class RateLimiter {
  public:
    static constexpr std::size_t kMaxTrackedKeys = 20000;

    bool allow(const std::string& key, std::size_t maxRequests, long long windowSeconds, long long nowSeconds);
    void clear();
    std::size_t trackedKeys();

  private:
    // Caller must hold mutex_.
    void evictLocked(long long windowSeconds, long long nowSeconds);

    std::mutex mutex_;
    std::unordered_map<std::string, std::deque<long long>> hits_;
};

} // namespace nuigraph
