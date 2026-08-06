#pragma once

#include "config/Config.hpp"
#include "storage/ConnectionPool.hpp"
#include "domain/Diagram.hpp"
#include "validation/DiagramValidator.hpp"

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <string>
#include <vector>

namespace nuigraph {

class PostgresStorage {
public:
    PostgresStorage(const Config& cfg, const DiagramValidator& validator);

    bool ping();
    std::vector<nlohmann::json> listDiagrams();
    std::vector<nlohmann::json> listDiagramsForOwner(const std::string& ownerTokenHash);
    Diagram getDiagram(long id);
    Diagram getDiagramBySlug(const std::string& slug);
    std::string ownerHashForDiagram(long id);
    long countDiagramsForOwner(const std::string& ownerTokenHash);
    Diagram createDiagram(const Diagram& input, bool importMode, const std::string& ownerTokenHash = "");
    Diagram updateDiagram(long id, const Diagram& input, const std::string& note, bool importMode);
    void deleteDiagram(long id);
    Diagram duplicateDiagram(long id, const std::string& ownerTokenHash = "");
    std::vector<VersionInfo> listVersions(long diagramId);
    VersionInfo createVersion(long diagramId, const std::string& note);
    Diagram restoreVersion(long diagramId, long versionId);

private:
    ConnectionPool pool_;
    Diagram getDiagram(pqxx::work& tx, long id);
    void replaceNodesAndEdges(pqxx::work& tx, long diagramId, const Diagram& diagram);
    VersionInfo createVersion(pqxx::work& tx, long diagramId, const std::string& note);
    void pruneVersions(pqxx::work& tx, long diagramId);
    std::string uniqueSlug(pqxx::work& tx, const std::string& title);

    const Config& cfg_;
    const DiagramValidator& validator_;
};

} // namespace nuigraph
