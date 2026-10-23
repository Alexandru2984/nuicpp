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

class PostgresStorage {
public:
    PostgresStorage(const Config& cfg, const DiagramValidator& validator);

    bool ping();
    OperationalMetrics operationalMetrics();
    std::vector<nlohmann::json> listDiagrams(long limit, long offset);
    std::vector<nlohmann::json> listDiagramsForOwner(const std::string& ownerTokenHash, long limit, long offset);
    Diagram getDiagram(long id);
    Diagram getDiagramBySlug(const std::string& slug);
    bool activeShareExists(const std::string& slug);
    std::string ownerHashForDiagram(long id);
    long countDiagrams();
    long countDiagramsForOwner(const std::string& ownerTokenHash);
    bool consumeRateLimit(const std::string& bucketKey, std::size_t maxRequests,
                          long long windowSeconds);
    void clearRateLimit(const std::string& bucketKey);
    Diagram createDiagram(const Diagram& input, bool importMode, const std::string& ownerTokenHash = "");
    Diagram updateDiagram(long id, const Diagram& input, const std::string& note,
                          bool importMode, long expectedRevision);
    Diagram updateSharing(long id, ShareAction action, long expectedRevision,
                          const ShareExpiry& expiry = {});
    void deleteDiagram(long id, long expectedRevision);
    Diagram duplicateDiagram(long id, const std::string& ownerTokenHash = "");
    Diagram forkDiagramBySlug(const std::string& slug, const std::string& ownerTokenHash);
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
