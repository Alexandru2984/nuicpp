#pragma once

#include "config/Config.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace nuigraph {

// Aggregate operational data only. These values deliberately contain no
// diagram titles, owner identifiers, IP addresses, request IDs or share tokens.
struct OperationalMetrics {
    bool databaseUp = false;
    std::uint64_t diagrams = 0;
    std::uint64_t trashedDiagrams = 0;
    std::uint64_t nodes = 0;
    std::uint64_t edges = 0;
    std::uint64_t versions = 0;
    std::uint64_t activeRateLimitBuckets = 0;
    std::uint64_t diagramCapacity = 0;
    std::uint64_t logicalBytes = 0;
    std::uint64_t logicalByteCapacity = 0;
    std::uint64_t rateLimitBucketCapacity = 0;
    std::uint64_t databaseBytes = 0;
    std::uint64_t poolMax = 0;
    std::uint64_t poolIdle = 0;
    std::uint64_t poolOutstanding = 0;
    std::uint64_t poolTimeouts = 0;
    std::uint64_t trashPurgeLastSuccessSeconds = 0;
};

class Metrics {
public:
    Metrics();

    void observeHttp(const std::string& method, int status, double seconds);
    void recordSecurityEvent(const std::string& event, const std::string& outcome);
    void recordDiagramCreated(const std::string& source);
    void recordReadiness(bool ready);
    void configureTrashRetention(int days, int intervalSeconds);
    void recordTrashPurge(std::uint64_t deleted);
    void recordTrashPurgeError();

    std::string render(FrontendShell shell, const OperationalMetrics& operational) const;

private:
    static constexpr std::size_t kMethodCount = 5;
    static constexpr std::size_t kStatusCount = 6;
    static constexpr std::size_t kLatencyBucketCount = 6;
    static constexpr std::size_t kSecurityEventCount = 34;
    static constexpr std::size_t kCreateSourceCount = 5;

    std::uint64_t startTimeSeconds_;
    std::array<std::atomic<std::uint64_t>, kMethodCount * kStatusCount> httpRequests_{};
    std::array<std::atomic<std::uint64_t>, kLatencyBucketCount> latencyBuckets_{};
    std::atomic<std::uint64_t> latencyCount_{0};
    std::atomic<double> latencySum_{0.0};
    std::array<std::atomic<std::uint64_t>, kSecurityEventCount> securityEvents_{};
    std::array<std::atomic<std::uint64_t>, kCreateSourceCount> diagramCreates_{};
    std::atomic<std::uint64_t> readinessSuccess_{0};
    std::atomic<std::uint64_t> readinessFailure_{0};
    std::atomic<int> readinessLast_{0};
    std::atomic<int> trashRetentionDays_{0};
    std::atomic<int> trashPurgeIntervalSeconds_{0};
    std::atomic<std::uint64_t> trashPurgeSuccess_{0};
    std::atomic<std::uint64_t> trashPurgeErrors_{0};
    std::atomic<std::uint64_t> trashPurged_{0};
};

} // namespace nuigraph
