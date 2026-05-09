#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace nuigraph {

class RateLimiter {
  public:
    bool allow(const std::string& key, std::size_t maxRequests, long long windowSeconds, long long nowSeconds);
    void clear();

  private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::deque<long long>> hits_;
};

} // namespace nuigraph
