#include "storage/PostgresStorage.hpp"

#include <openssl/rand.h>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace nuigraph {
namespace {

std::string slugBase(const std::string& title) {
    std::string out;
    for (char c : title) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (!out.empty() && out.back() != '-') {
            out.push_back('-');
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    if (out.empty()) {
        out = "diagram";
    }
    if (out.size() > 80) {
        out.resize(80);
    }
    return out;
}

std::string rowString(const pqxx::row& row, const char* key) {
    return row[key].is_null() ? "" : row[key].as<std::string>();
}

std::string randomHex(std::size_t bytes) {
    std::vector<unsigned char> data(bytes);
    if (RAND_bytes(data.data(), static_cast<int>(data.size())) != 1) {
        throw std::runtime_error("random generator failed");
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto byte : data) {
        out << std::setw(2) << static_cast<int>(byte);
    }
    return out.str();
}

nlohmann::json diagramSummaryJson(const pqxx::row& row) {
    return {
        {"id", row["id"].as<long>()},
        {"title", rowString(row, "title")},
        {"slug", rowString(row, "slug")},
        {"description", rowString(row, "description")},
        {"created_at", rowString(row, "created_at")},
        {"updated_at", rowString(row, "updated_at")},
        {"node_count", row["node_count"].as<int>()},
        {"edge_count", row["edge_count"].as<int>()}
    };
}

} // namespace

PostgresStorage::PostgresStorage(const Config& cfg, const DiagramValidator& validator)
    : pool_(cfg.databaseUrl, static_cast<std::size_t>(cfg.dbPoolSize)),
      cfg_(cfg), validator_(validator) {}

bool PostgresStorage::ping() {
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    auto result = tx.exec("SELECT 1");
    tx.commit();
    return !result.empty();
}

std::string PostgresStorage::uniqueSlug(pqxx::work& tx, const std::string& title) {
    auto base = slugBase(title);
    std::string slug = base + "-" + randomHex(8);
    int i = 2;
    while (!tx.exec("SELECT 1 FROM diagrams WHERE slug=$1", pqxx::params{slug}).empty()) {
        slug = base + "-" + randomHex(8) + "-" + std::to_string(i++);
    }
    return slug;
}

// The ordering is (updated_at DESC, id DESC) and id is unique, so it is a total
// order. That matters for paging: without the id tiebreak, two diagrams saved in
// the same second could swap places between two page fetches and one of them
// would be skipped or shown twice.
std::vector<nlohmann::json> PostgresStorage::listDiagrams(long limit, long offset) {
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    auto rows = tx.exec(R"SQL(
        SELECT d.id, d.title, d.slug, d.description,
               to_char(d.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at,
               to_char(d.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS updated_at,
               (SELECT count(*) FROM nodes n WHERE n.diagram_id=d.id) AS node_count,
               (SELECT count(*) FROM edges e WHERE e.diagram_id=d.id) AS edge_count
        FROM diagrams d
        ORDER BY d.updated_at DESC, d.id DESC
        LIMIT $1 OFFSET $2
    )SQL", pqxx::params{limit, offset});
    std::vector<nlohmann::json> out;
    for (const auto& row : rows) {
        out.push_back(diagramSummaryJson(row));
    }
    tx.commit();
    return out;
}

std::vector<nlohmann::json> PostgresStorage::listDiagramsForOwner(const std::string& ownerTokenHash, long limit, long offset) {
    if (ownerTokenHash.empty()) {
        return {};
    }
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    auto rows = tx.exec(R"SQL(
        SELECT d.id, d.title, d.slug, d.description,
               to_char(d.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at,
               to_char(d.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS updated_at,
               (SELECT count(*) FROM nodes n WHERE n.diagram_id=d.id) AS node_count,
               (SELECT count(*) FROM edges e WHERE e.diagram_id=d.id) AS edge_count
        FROM diagrams d
        WHERE d.owner_token_hash=$1
        ORDER BY d.updated_at DESC, d.id DESC
        LIMIT $2 OFFSET $3
    )SQL", pqxx::params{ownerTokenHash, limit, offset});
    std::vector<nlohmann::json> out;
    for (const auto& row : rows) {
        out.push_back(diagramSummaryJson(row));
    }
    tx.commit();
    return out;
}

Diagram PostgresStorage::getDiagram(long id) {
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    auto diagram = getDiagram(tx, id);
    tx.commit();
    return diagram;
}

Diagram PostgresStorage::getDiagramBySlug(const std::string& slug) {
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    auto rows = tx.exec("SELECT id FROM diagrams WHERE slug=$1", pqxx::params{slug});
    if (rows.empty()) {
        throw std::runtime_error("diagram not found");
    }
    auto diagram = getDiagram(tx, rows[0]["id"].as<long>());
    tx.commit();
    return diagram;
}

std::string PostgresStorage::ownerHashForDiagram(long id) {
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    auto rows = tx.exec("SELECT owner_token_hash FROM diagrams WHERE id=$1", pqxx::params{id});
    if (rows.empty()) {
        throw std::runtime_error("diagram not found");
    }
    auto owner = rows[0]["owner_token_hash"].is_null() ? "" : rows[0]["owner_token_hash"].as<std::string>();
    tx.commit();
    return owner;
}

long PostgresStorage::countDiagrams() {
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    auto rows = tx.exec("SELECT count(*) AS n FROM diagrams");
    auto count = rows[0]["n"].as<long>();
    tx.commit();
    return count;
}

long PostgresStorage::countDiagramsForOwner(const std::string& ownerTokenHash) {
    if (ownerTokenHash.empty()) {
        return 0;
    }
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    auto rows = tx.exec("SELECT count(*) AS n FROM diagrams WHERE owner_token_hash=$1", pqxx::params{ownerTokenHash});
    auto count = rows[0]["n"].as<long>();
    tx.commit();
    return count;
}

Diagram PostgresStorage::getDiagram(pqxx::work& tx, long id) {
    auto rows = tx.exec(R"SQL(
        SELECT id, title, slug, description,
               to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at,
               to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS updated_at
        FROM diagrams WHERE id=$1
    )SQL", pqxx::params{id});
    if (rows.empty()) {
        throw std::runtime_error("diagram not found");
    }
    Diagram diagram;
    const auto& d = rows[0];
    diagram.id = d["id"].as<long>();
    diagram.title = rowString(d, "title");
    diagram.slug = rowString(d, "slug");
    diagram.description = rowString(d, "description");
    diagram.createdAt = rowString(d, "created_at");
    diagram.updatedAt = rowString(d, "updated_at");

    auto nodeRows = tx.exec(R"SQL(
        SELECT node_key, type, title, x, y, width, height, color, metadata_json::text
        FROM nodes WHERE diagram_id=$1 ORDER BY id ASC
    )SQL", pqxx::params{id});
    for (const auto& row : nodeRows) {
        Node node;
        node.key = rowString(row, "node_key");
        node.type = rowString(row, "type");
        node.title = rowString(row, "title");
        node.x = row["x"].as<double>();
        node.y = row["y"].as<double>();
        node.width = row["width"].as<double>();
        node.height = row["height"].as<double>();
        node.color = rowString(row, "color");
        node.metadata = nlohmann::json::parse(rowString(row, "metadata_json"));
        diagram.nodes.push_back(node);
    }

    auto edgeRows = tx.exec(R"SQL(
        SELECT edge_key, source_node_key, target_node_key, label, directed, color, metadata_json::text
        FROM edges WHERE diagram_id=$1 ORDER BY id ASC
    )SQL", pqxx::params{id});
    for (const auto& row : edgeRows) {
        Edge edge;
        edge.key = rowString(row, "edge_key");
        edge.source = rowString(row, "source_node_key");
        edge.target = rowString(row, "target_node_key");
        edge.label = rowString(row, "label");
        edge.directed = row["directed"].as<bool>();
        edge.color = rowString(row, "color");
        edge.metadata = nlohmann::json::parse(rowString(row, "metadata_json"));
        diagram.edges.push_back(edge);
    }
    return diagram;
}

void PostgresStorage::replaceNodesAndEdges(pqxx::work& tx, long diagramId, const Diagram& diagram) {
    tx.exec("DELETE FROM edges WHERE diagram_id=$1", pqxx::params{diagramId});
    tx.exec("DELETE FROM nodes WHERE diagram_id=$1", pqxx::params{diagramId});
    for (const auto& node : diagram.nodes) {
        tx.exec(R"SQL(
            INSERT INTO nodes (diagram_id, node_key, type, title, x, y, width, height, color, metadata_json)
            VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10::jsonb)
        )SQL", pqxx::params{diagramId, node.key, node.type, node.title, node.x, node.y, node.width, node.height, node.color, node.metadata.dump()});
    }
    for (const auto& edge : diagram.edges) {
        tx.exec(R"SQL(
            INSERT INTO edges (diagram_id, edge_key, source_node_key, target_node_key, label, directed, color, metadata_json)
            VALUES ($1,$2,$3,$4,$5,$6,$7,$8::jsonb)
        )SQL", pqxx::params{diagramId, edge.key, edge.source, edge.target, edge.label, edge.directed, edge.color, edge.metadata.dump()});
    }
}

Diagram PostgresStorage::createDiagram(const Diagram& input, bool importMode, const std::string& ownerTokenHash) {
    Diagram diagram = input;
    auto validation = validator_.validate(diagram, importMode);
    if (!validation.ok) {
        throw std::runtime_error(validation.errors.front());
    }
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    auto slug = uniqueSlug(tx, diagram.title);
    auto rows = tx.exec(R"SQL(
        INSERT INTO diagrams (title, slug, description, owner_token_hash)
        VALUES ($1,$2,$3,$4)
        RETURNING id
    )SQL", pqxx::params{diagram.title, slug, diagram.description, ownerTokenHash});
    long id = rows[0]["id"].as<long>();
    replaceNodesAndEdges(tx, id, diagram);
    createVersion(tx, id, "initial snapshot");
    tx.commit();
    return getDiagram(id);
}

Diagram PostgresStorage::updateDiagram(long id, const Diagram& input, const std::string& note, bool importMode) {
    Diagram diagram = input;
    diagram.id = id;
    auto validation = validator_.validate(diagram, importMode);
    if (!validation.ok) {
        throw std::runtime_error(validation.errors.front());
    }
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    auto exists = tx.exec("SELECT 1 FROM diagrams WHERE id=$1", pqxx::params{id});
    if (exists.empty()) {
        throw std::runtime_error("diagram not found");
    }
    tx.exec("UPDATE diagrams SET title=$1, description=$2, updated_at=now() WHERE id=$3", pqxx::params{diagram.title, diagram.description, id});
    replaceNodesAndEdges(tx, id, diagram);
    createVersion(tx, id, note.empty() ? "saved snapshot" : note);
    tx.commit();
    return getDiagram(id);
}

void PostgresStorage::deleteDiagram(long id) {
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    tx.exec("DELETE FROM diagrams WHERE id=$1", pqxx::params{id});
    tx.commit();
}

Diagram PostgresStorage::duplicateDiagram(long id, const std::string& ownerTokenHash) {
    auto source = getDiagram(id);
    source.id = 0;
    source.title += " Copy";
    source.slug.clear();
    return createDiagram(source, false, ownerTokenHash);
}

std::vector<VersionInfo> PostgresStorage::listVersions(long diagramId) {
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    auto rows = tx.exec(R"SQL(
        SELECT id, diagram_id, version_number,
               to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at,
               note
        FROM diagram_versions
        WHERE diagram_id=$1
        ORDER BY version_number DESC
    )SQL", pqxx::params{diagramId});
    std::vector<VersionInfo> out;
    for (const auto& row : rows) {
        out.push_back({row["id"].as<long>(), row["diagram_id"].as<long>(), row["version_number"].as<int>(), rowString(row, "created_at"), rowString(row, "note")});
    }
    tx.commit();
    return out;
}

VersionInfo PostgresStorage::createVersion(long diagramId, const std::string& note) {
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    auto info = createVersion(tx, diagramId, note);
    tx.commit();
    return info;
}

VersionInfo PostgresStorage::createVersion(pqxx::work& tx, long diagramId, const std::string& note) {
    auto diagram = getDiagram(tx, diagramId);
    auto next = tx.exec("SELECT COALESCE(MAX(version_number),0)+1 AS n FROM diagram_versions WHERE diagram_id=$1", pqxx::params{diagramId})[0]["n"].as<int>();
    auto rows = tx.exec(R"SQL(
        INSERT INTO diagram_versions (diagram_id, version_number, snapshot_json, note)
        VALUES ($1,$2,$3::jsonb,$4)
        RETURNING id, diagram_id, version_number,
                  to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at,
                  note
    )SQL", pqxx::params{diagramId, next, diagramToJson(diagram).dump(), note});
    pruneVersions(tx, diagramId);
    const auto& row = rows[0];
    return {row["id"].as<long>(), row["diagram_id"].as<long>(), row["version_number"].as<int>(), rowString(row, "created_at"), rowString(row, "note")};
}

void PostgresStorage::pruneVersions(pqxx::work& tx, long diagramId) {
    tx.exec(R"SQL(
        DELETE FROM diagram_versions
        WHERE diagram_id=$1
          AND id NOT IN (
              SELECT id FROM diagram_versions
              WHERE diagram_id=$1
              ORDER BY version_number DESC
              LIMIT $2
          )
    )SQL", pqxx::params{diagramId, cfg_.maxVersionsPerDiagram});
}

Diagram PostgresStorage::restoreVersion(long diagramId, long versionId) {
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    auto rows = tx.exec("SELECT snapshot_json::text FROM diagram_versions WHERE diagram_id=$1 AND id=$2", pqxx::params{diagramId, versionId});
    if (rows.empty()) {
        throw std::runtime_error("version not found");
    }
    auto snapshot = nlohmann::json::parse(rows[0][0].as<std::string>());
    Diagram diagram = diagramFromJson(snapshot);
    diagram.id = diagramId;
    auto validation = validator_.validate(diagram, false);
    if (!validation.ok) {
        throw std::runtime_error(validation.errors.front());
    }
    tx.exec("UPDATE diagrams SET title=$1, description=$2, updated_at=now() WHERE id=$3", pqxx::params{diagram.title, diagram.description, diagramId});
    replaceNodesAndEdges(tx, diagramId, diagram);
    createVersion(tx, diagramId, "restored version " + std::to_string(versionId));
    tx.commit();
    return getDiagram(diagramId);
}

} // namespace nuigraph
