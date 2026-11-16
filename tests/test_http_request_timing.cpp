#include "observability/HttpRequestTiming.hpp"

#include <array>
#include <barrier>
#include <cmath>
#include <iostream>
#include <thread>

namespace {

bool expectDuration(double actual, double expected, const char* message) {
    if (std::abs(actual - expected) < 1e-9) {
        return true;
    }
    std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
    return false;
}

} // namespace

int main() {
    using namespace std::chrono_literals;
    using nuigraph::httpRequestTiming;
    const nuigraph::HttpRequestTiming::Clock::time_point start{};
    bool ok = true;

    ok &= expectDuration(httpRequestTiming.finish("missing", start), 0.0,
                         "requests that bypass routing have no timer");
    httpRequestTiming.begin("first", start);
    ok &= expectDuration(httpRequestTiming.finish("first", start + 25ms), 0.025,
                         "routed responses retain their elapsed time");
    ok &= expectDuration(httpRequestTiming.finish("first", start + 1s), 0.0,
                         "a response consumes its timer once");

    httpRequestTiming.begin("failed-write", start);
    ok &= expectDuration(httpRequestTiming.finish("", start + 2s), 0.0,
                         "parser errors cannot inherit an abandoned timer");
    ok &= expectDuration(httpRequestTiming.finish("failed-write", start + 3s), 0.0,
                         "an unmatched response clears abandoned timing state");

    httpRequestTiming.begin("old-request", start);
    httpRequestTiming.begin("new-request", start + 1s);
    ok &= expectDuration(httpRequestTiming.finish("new-request", start + 1010ms), 0.010,
                         "each routed request replaces abandoned timing state");

    httpRequestTiming.begin("reused-id", start);
    ok &= expectDuration(httpRequestTiming.finish("different-id", start + 1s), 0.0,
                         "a different response ID cannot use an active timer");
    httpRequestTiming.begin("reused-id", start + 2s);
    ok &= expectDuration(httpRequestTiming.finish("reused-id", start + 2030ms), 0.030,
                         "consecutive requests may reuse a caller-provided ID");

    std::barrier ready(3);
    std::array<double, 2> elapsed{};
    std::array<std::jthread, 2> workers;
    httpRequestTiming.begin("same-id", start);
    for (std::size_t i = 0; i < workers.size(); ++i) {
        workers[i] = std::jthread([&, i] {
            httpRequestTiming.begin("same-id", start + (i + 1) * 1s);
            ready.arrive_and_wait();
            elapsed[i] = httpRequestTiming.finish("same-id", start + 5s);
        });
    }
    ready.arrive_and_wait();
    ok &= expectDuration(httpRequestTiming.finish("same-id", start + 5s), 5.0,
                         "worker requests cannot overwrite another thread's timer");
    for (auto& worker : workers) {
        worker.join();
    }
    ok &= expectDuration(elapsed[0], 4.0, "first worker retains its own start time");
    ok &= expectDuration(elapsed[1], 3.0, "second worker retains its own start time");

    if (!ok) {
        return 1;
    }
    std::cout << "HTTP request timing passed\n";
}
