#include "observability/Metrics.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace nuigraph {
namespace {

constexpr std::array<const char*, 5> kMethods = {"GET", "POST", "PUT", "DELETE", "OTHER"};
constexpr std::array<const char*, 6> kStatuses = {"1xx", "2xx", "3xx", "4xx", "5xx", "other"};
constexpr std::array<double, 6> kLatencyBounds = {0.005, 0.025, 0.1, 0.25, 1.0, 5.0};
constexpr std::array<const char*, 6> kLatencyLabels = {"0.005", "0.025", "0.1", "0.25", "1", "5"};

struct SecurityEventLabel {
    const char* event;
    const char* outcome;
};

constexpr std::array<SecurityEventLabel, 22> kSecurityEvents = {{
    {"csrf_rejected", "blocked"},
    {"write_rate_limited", "blocked"},
    {"create_rate_limited", "blocked"},
    {"rate_limit_storage", "error"},
    {"rate_limit_storage", "clear_error"},
    {"authorization_read_denied", "blocked"},
    {"authorization_write_denied", "blocked"},
    {"revision_conflict", "blocked"},
    {"global_capacity_exhausted", "blocked"},
    {"owner_quota_exhausted", "blocked"},
    {"storage_failure", "error"},
    {"authentication_required", "blocked"},
    {"admin_login", "rate_limited"},
    {"admin_login", "failed"},
    {"admin_login", "succeeded"},
    {"share_view", "allowed"},
    {"share_view", "blocked"},
    {"sharing_change", "enabled"},
    {"sharing_change", "disabled"},
    {"sharing_change", "rotated"},
    {"sharing_change", "expiry_updated"},
    {"session_rekey", "succeeded"}
}};

constexpr std::array<const char*, 5> kCreateSources = {"blank", "template", "duplicate", "import", "fork"};

std::size_t methodIndex(const std::string& method) {
    for (std::size_t i = 0; i + 1 < kMethods.size(); ++i) {
        if (method == kMethods[i]) {
            return i;
        }
    }
    return kMethods.size() - 1;
}

std::size_t statusIndex(int status) {
    if (status >= 100 && status <= 599) {
        return static_cast<std::size_t>(status / 100 - 1);
    }
    return kStatuses.size() - 1;
}

} // namespace

Metrics::Metrics()
    : startTimeSeconds_(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count())) {}

void Metrics::observeHttp(const std::string& method, int status, double seconds) {
    const auto requestIndex = methodIndex(method) * kStatusCount + statusIndex(status);
    httpRequests_[requestIndex].fetch_add(1, std::memory_order_relaxed);

    const auto safeSeconds = std::max(0.0, seconds);
    for (std::size_t i = 0; i < kLatencyBounds.size(); ++i) {
        if (safeSeconds <= kLatencyBounds[i]) {
            latencyBuckets_[i].fetch_add(1, std::memory_order_relaxed);
        }
    }
    latencyCount_.fetch_add(1, std::memory_order_relaxed);
    latencySum_.fetch_add(safeSeconds, std::memory_order_relaxed);
}

void Metrics::recordSecurityEvent(const std::string& event, const std::string& outcome) {
    for (std::size_t i = 0; i < kSecurityEvents.size(); ++i) {
        if (event == kSecurityEvents[i].event && outcome == kSecurityEvents[i].outcome) {
            securityEvents_[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void Metrics::recordDiagramCreated(const std::string& source) {
    for (std::size_t i = 0; i < kCreateSources.size(); ++i) {
        if (source == kCreateSources[i]) {
            diagramCreates_[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void Metrics::recordReadiness(bool ready) {
    readinessLast_.store(ready ? 1 : 0, std::memory_order_relaxed);
    (ready ? readinessSuccess_ : readinessFailure_).fetch_add(1, std::memory_order_relaxed);
}

std::string Metrics::render(FrontendShell shell, const OperationalMetrics& operational) const {
    std::ostringstream out;
    out << std::setprecision(17);
    out << "# HELP nuigraph_build_info Static build and frontend-shell information.\n"
        << "# TYPE nuigraph_build_info gauge\n"
        << "nuigraph_build_info{frontend=\"" << frontendShellName(shell) << "\"} 1\n"
        << "# HELP nuigraph_process_start_time_seconds Unix time when this process started.\n"
        << "# TYPE nuigraph_process_start_time_seconds gauge\n"
        << "nuigraph_process_start_time_seconds " << startTimeSeconds_ << "\n"
        << "# HELP nuigraph_http_requests_total Completed HTTP responses by bounded method and status class.\n"
        << "# TYPE nuigraph_http_requests_total counter\n";

    for (std::size_t method = 0; method < kMethods.size(); ++method) {
        for (std::size_t status = 0; status < kStatuses.size(); ++status) {
            const auto value = httpRequests_[method * kStatusCount + status].load(std::memory_order_relaxed);
            out << "nuigraph_http_requests_total{method=\"" << kMethods[method]
                << "\",status=\"" << kStatuses[status] << "\"} " << value << "\n";
        }
    }

    out << "# HELP nuigraph_http_request_duration_seconds End-to-end HTTP response latency.\n"
        << "# TYPE nuigraph_http_request_duration_seconds histogram\n";
    for (std::size_t i = 0; i < kLatencyBounds.size(); ++i) {
        out << "nuigraph_http_request_duration_seconds_bucket{le=\"" << kLatencyLabels[i]
            << "\"} " << latencyBuckets_[i].load(std::memory_order_relaxed) << "\n";
    }
    const auto latencyCount = latencyCount_.load(std::memory_order_relaxed);
    out << "nuigraph_http_request_duration_seconds_bucket{le=\"+Inf\"} " << latencyCount << "\n"
        << "nuigraph_http_request_duration_seconds_sum " << latencySum_.load(std::memory_order_relaxed) << "\n"
        << "nuigraph_http_request_duration_seconds_count " << latencyCount << "\n"
        << "# HELP nuigraph_security_events_total Bounded security-relevant outcomes.\n"
        << "# TYPE nuigraph_security_events_total counter\n";
    for (std::size_t i = 0; i < kSecurityEvents.size(); ++i) {
        out << "nuigraph_security_events_total{event=\"" << kSecurityEvents[i].event
            << "\",outcome=\"" << kSecurityEvents[i].outcome << "\"} "
            << securityEvents_[i].load(std::memory_order_relaxed) << "\n";
    }

    out << "# HELP nuigraph_diagram_creates_total Successfully persisted diagrams by bounded source.\n"
        << "# TYPE nuigraph_diagram_creates_total counter\n";
    for (std::size_t i = 0; i < kCreateSources.size(); ++i) {
        out << "nuigraph_diagram_creates_total{source=\"" << kCreateSources[i] << "\"} "
            << diagramCreates_[i].load(std::memory_order_relaxed) << "\n";
    }

    out << "# HELP nuigraph_readiness_checks_total Readiness probe results.\n"
        << "# TYPE nuigraph_readiness_checks_total counter\n"
        << "nuigraph_readiness_checks_total{outcome=\"ready\"} "
        << readinessSuccess_.load(std::memory_order_relaxed) << "\n"
        << "nuigraph_readiness_checks_total{outcome=\"unavailable\"} "
        << readinessFailure_.load(std::memory_order_relaxed) << "\n"
        << "# HELP nuigraph_readiness_last_result Whether the last readiness probe succeeded.\n"
        << "# TYPE nuigraph_readiness_last_result gauge\n"
        << "nuigraph_readiness_last_result " << readinessLast_.load(std::memory_order_relaxed) << "\n"
        << "# HELP nuigraph_database_up Whether aggregate database telemetry was collected.\n"
        << "# TYPE nuigraph_database_up gauge\n"
        << "nuigraph_database_up " << (operational.databaseUp ? 1 : 0) << "\n"
        << "# HELP nuigraph_storage_objects Current aggregate database object counts.\n"
        << "# TYPE nuigraph_storage_objects gauge\n"
        << "nuigraph_storage_objects{kind=\"diagrams\"} " << operational.diagrams << "\n"
        << "nuigraph_storage_objects{kind=\"nodes\"} " << operational.nodes << "\n"
        << "nuigraph_storage_objects{kind=\"edges\"} " << operational.edges << "\n"
        << "nuigraph_storage_objects{kind=\"versions\"} " << operational.versions << "\n"
        << "nuigraph_storage_objects{kind=\"rate_limit_buckets\"} "
        << operational.activeRateLimitBuckets << "\n"
        << "# HELP nuigraph_storage_capacity Configured bounded object capacity.\n"
        << "# TYPE nuigraph_storage_capacity gauge\n"
        << "nuigraph_storage_capacity{kind=\"diagrams\"} " << operational.diagramCapacity << "\n"
        << "nuigraph_storage_capacity{kind=\"rate_limit_buckets\"} "
        << operational.rateLimitBucketCapacity << "\n"
        << "# HELP nuigraph_logical_storage_bytes Current bounded user-controlled logical bytes.\n"
        << "# TYPE nuigraph_logical_storage_bytes gauge\n"
        << "nuigraph_logical_storage_bytes " << operational.logicalBytes << "\n"
        << "# HELP nuigraph_logical_storage_capacity_bytes Configured logical-byte ceiling.\n"
        << "# TYPE nuigraph_logical_storage_capacity_bytes gauge\n"
        << "nuigraph_logical_storage_capacity_bytes " << operational.logicalByteCapacity << "\n"
        << "# HELP nuigraph_database_size_bytes Current PostgreSQL database size.\n"
        << "# TYPE nuigraph_database_size_bytes gauge\n"
        << "nuigraph_database_size_bytes " << operational.databaseBytes << "\n"
        << "# HELP nuigraph_db_pool_connections Connection pool slots by state.\n"
        << "# TYPE nuigraph_db_pool_connections gauge\n"
        << "nuigraph_db_pool_connections{state=\"max\"} " << operational.poolMax << "\n"
        << "nuigraph_db_pool_connections{state=\"idle\"} " << operational.poolIdle << "\n"
        << "nuigraph_db_pool_connections{state=\"outstanding\"} " << operational.poolOutstanding << "\n"
        << "# HELP nuigraph_db_pool_timeouts_total Requests that timed out waiting for a database connection.\n"
        << "# TYPE nuigraph_db_pool_timeouts_total counter\n"
        << "nuigraph_db_pool_timeouts_total " << operational.poolTimeouts << "\n";
    return out.str();
}

} // namespace nuigraph
