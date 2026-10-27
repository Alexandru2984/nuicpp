#pragma once

#include "config/Config.hpp"
#include "storage/ConnectionPool.hpp"
#include "domain/Diagram.hpp"
#include "observability/Metrics.hpp"
#include "validation/DiagramValidator.hpp"

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace nuigraph {

class DiagramQuotaExceeded final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class DiagramCapacityExceeded final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class IdempotencyConflict final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct IdempotencyRequest {
    std::string principalKey;
    std::string operation;
    std::string keyHash;
    std::string requestHash;
};

struct DiagramCreationResult {
    Diagram diagram;
    bool replayed = false;
};

struct TrashPurgeResult {
    bool claimed = false;
    std::uint64_t deleted = 0;
};

class DiagramRevisionConflict final : public std::runtime_error {
public:
    DiagramRevisionConflict(long currentRevision, const std::string& message)
        : std::runtime_error(message), currentRevision_(currentRevision) {}

    long currentRevision() const noexcept { return currentRevision_; }

private:
    long currentRevision_;
};

enum class ShareAction {
    Enable,
    Disable,
    Rotate,
    SetExpiry
};

struct ShareExpiry {
    bool preserve = false;
    std::optional<long> seconds;
};

struct ShareAccessPolicy {
    bool passwordProtected = false;
    long passwordVersion = 0;
    std::string passwordHash;
};

class PostgresStorage {
public:
    PostgresStorage(const Config& cfg, const DiagramValidator& validator);

    bool ping();
    OperationalMetrics operationalMetrics();
    std::vector<nlohmann::json> listDiagrams(long limit, long offset);
    std::vector<nlohmann::json> listDiagramsForOwner(const std::string& ownerTokenHash, long limit, long offset);
    std::vector<nlohmann::json> listDeletedDiagrams(long limit, long offset);
    std::vector<nlohmann::json> listDeletedDiagramsForOwner(
        const std::string& ownerTokenHash, long limit, long offset);
    Diagram getDiagram(long id);
    Diagram getDiagramBySlug(const std::string& slug,
                             long authorizedPasswordVersion = -1);
    std::optional<ShareAccessPolicy> shareAccessPolicy(const std::string& slug);
    std::string ownerHashForDiagram(long id);
    std::string ownerHashForDeletedDiagram(long id);
    long countDiagrams();
    long countDiagramsForOwner(const std::string& ownerTokenHash);
    long countDeletedDiagrams();
    long countDeletedDiagramsForOwner(const std::string& ownerTokenHash);
    nlohmann::json exportDataForOwner(const std::string& ownerTokenHash);
    std::uint64_t deleteAllDataForOwner(const std::string& ownerTokenHash);
    bool consumeRateLimit(const std::string& bucketKey, std::size_t maxRequests,
                          long long windowSeconds);
    void clearRateLimit(const std::string& bucketKey);
    bool consumeAdminTotpCounter(long long counter);
    DiagramCreationResult createDiagram(
        const Diagram& input, bool importMode, const std::string& ownerTokenHash = "",
        const std::optional<IdempotencyRequest>& idempotency = std::nullopt);
    Diagram updateDiagram(long id, const Diagram& input, const std::string& note,
                          bool importMode, long expectedRevision);
    Diagram updateSharing(long id, ShareAction action, long expectedRevision,
                          const ShareExpiry& expiry = {});
    Diagram updateSharePassword(long id, long expectedRevision,
                                const std::optional<std::string>& passwordHash);
    void trashDiagram(long id, long expectedRevision);
    Diagram restoreDiagramFromTrash(long id, long expectedRevision);
    void permanentlyDeleteDiagram(long id, long expectedRevision);
    TrashPurgeResult purgeExpiredTrash(int retentionDays, int batchSize,
                                       int intervalSeconds);
    DiagramCreationResult duplicateDiagram(
        long id, const std::string& ownerTokenHash = "",
        const std::optional<IdempotencyRequest>& idempotency = std::nullopt);
    DiagramCreationResult forkDiagramBySlug(
        const std::string& slug, const std::string& ownerTokenHash,
        const std::optional<IdempotencyRequest>& idempotency = std::nullopt,
        long authorizedPasswordVersion = -1);
    std::vector<VersionInfo> listVersions(long diagramId);
    VersionInfo createVersion(long diagramId, const std::string& note);
    Diagram getVersion(long diagramId, long versionId);
    Diagram restoreVersion(long diagramId, long versionId, long expectedRevision);

private:
    ConnectionPool pool_;
    Diagram getDiagram(pqxx::work& tx, long id);
    void replaceNodesAndEdges(pqxx::work& tx, long diagramId, const Diagram& diagram);
    VersionInfo createVersion(pqxx::work& tx, long diagramId, const std::string& note);
    void pruneVersions(pqxx::work& tx, long diagramId);
    void enforceLogicalStorageCapacity(pqxx::work& tx,
                                       const std::string& ownerTokenHash);
    std::string uniqueSlug(pqxx::work& tx, const std::string& title);

    const Config& cfg_;
    const DiagramValidator& validator_;
};

} // namespace nuigraph
