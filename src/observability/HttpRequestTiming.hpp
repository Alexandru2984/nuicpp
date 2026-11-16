#pragma once

#include <chrono>
#include <string>

namespace nuigraph {

class HttpRequestTiming {
public:
    using Clock = std::chrono::steady_clock;

    void begin(const std::string& requestId, Clock::time_point now = Clock::now()) {
        requestId_ = requestId;
        startedAt_ = now;
    }

    double finish(const std::string& requestId, Clock::time_point now = Clock::now()) {
        const bool matches = !requestId_.empty() && requestId_ == requestId;
        requestId_.clear();
        return matches ? std::chrono::duration<double>(now - startedAt_).count() : 0.0;
    }

private:
    std::string requestId_;
    Clock::time_point startedAt_{};
};

// cpp-httplib routes and logs each request on the same worker thread. Match the
// response's generated ID as well, so parser errors that bypass routing cannot
// inherit a previous request's timer if its response failed before logging.
inline thread_local HttpRequestTiming httpRequestTiming;

} // namespace nuigraph
