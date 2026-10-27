#include "storage/PostgresStorage.hpp"

#include <openssl/rand.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace nuigraph {
namespace {

constexpr long kMaxPersistentRateLimitKeys = 20000;
const std::vector<std::string> kIdempotentCreateOperations = {
    "create", "template", "fork", "duplicate", "import"
};

bool isLowerHex(const std::string& value, std::size_t length) {
    return value.size() == length &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
}

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

std::uint64_t logicalDiagramBytes(const Diagram& diagram) {
    std::uint64_t total = diagram.title.size() + diagram.description.size();
    for (const auto& node : diagram.nodes) {
        total += node.key.size() + node.type.size() + node.title.size()
            + node.color.size() + node.metadata.dump().size();
    }
    for (const auto& edge : diagram.edges) {
        total += edge.key.size() + edge.source.size() + edge.target.size()
            + edge.label.size() + edge.color.size() + edge.metadata.dump().size();
    }
    return std::max<std::uint64_t>(1, total);
}

nlohmann::json diagramSummaryJson(const pqxx::row& row) {
    const auto sharingEnabled = row["sharing_enabled"].as<bool>();
    nlohmann::json result = {
        {"id", row["id"].as<long>()},
        {"title", rowString(row, "title")},
        {"slug", sharingEnabled ? rowString(row, "slug") : ""},
        {"sharing_enabled", sharingEnabled},
        {"share_expired", row["share_expired"].as<bool>()},
        {"share_password_protected", row["share_password_protected"].as<bool>()},
        {"description", rowString(row, "description")},
        {"created_at", rowString(row, "created_at")},
        {"updated_at", rowString(row, "updated_at")},
        {"deleted_at", row["deleted_at"].is_null()
            ? nlohmann::json(nullptr)
            : nlohmann::json(rowString(row, "deleted_at"))},
        {"revision", row["revision"].as<long>()},
        {"node_count", row["node_count"].as<int>()},
        {"edge_count", row["edge_count"].as<int>()}
    };
    result["share_expires_at"] = row["share_expires_at"].is_null()
        ? nlohmann::json(nullptr)
        : nlohmann::json(rowString(row, "share_expires_at"));
    return result;
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

OperationalMetrics PostgresStorage::operationalMetrics() {
    OperationalMetrics metrics;
    metrics.diagramCapacity = static_cast<std::uint64_t>(cfg_.maxDiagramsTotal);
    metrics.logicalByteCapacity = cfg_.maxLogicalBytesTotal;
    metrics.rateLimitBucketCapacity = static_cast<std::uint64_t>(kMaxPersistentRateLimitKeys);
    try {
        {
            auto conn = pool_.acquire();
            pqxx::read_transaction tx(conn.get());
            const auto row = tx.exec(R"SQL(
                SELECT
                    (SELECT count(*) FROM diagrams) AS diagrams,
                    (SELECT count(*) FROM diagrams WHERE deleted_at IS NOT NULL) AS trashed_diagrams,
                    COALESCE((SELECT n_live_tup FROM pg_stat_user_tables
                              WHERE schemaname='public' AND relname='nodes'), 0) AS nodes,
                    COALESCE((SELECT n_live_tup FROM pg_stat_user_tables
                              WHERE schemaname='public' AND relname='edges'), 0) AS edges,
                    COALESCE((SELECT n_live_tup FROM pg_stat_user_tables
                              WHERE schemaname='public' AND relname='diagram_versions'), 0) AS versions,
                    (SELECT count(*) FROM rate_limit_buckets) AS rate_limit_buckets,
                    (SELECT logical_bytes FROM public.logical_storage_usage
                     WHERE singleton=true) AS logical_bytes,
                    COALESCE((SELECT extract(epoch FROM last_success_at)::bigint
                              FROM public.maintenance_jobs
                              WHERE job_name='trash_purge'), 0) AS trash_purge_last_success,
                    pg_database_size(current_database()) AS database_bytes
            )SQL")[0];
            metrics.diagrams = row["diagrams"].as<std::uint64_t>();
            metrics.trashedDiagrams = row["trashed_diagrams"].as<std::uint64_t>();
            metrics.nodes = row["nodes"].as<std::uint64_t>();
            metrics.edges = row["edges"].as<std::uint64_t>();
            metrics.versions = row["versions"].as<std::uint64_t>();
            metrics.activeRateLimitBuckets = row["rate_limit_buckets"].as<std::uint64_t>();
            metrics.logicalBytes = row["logical_bytes"].as<std::uint64_t>();
            metrics.trashPurgeLastSuccessSeconds =
                row["trash_purge_last_success"].as<std::uint64_t>();
            metrics.databaseBytes = row["database_bytes"].as<std::uint64_t>();
            metrics.databaseUp = true;
        }
    } catch (const std::exception&) {
        // Metrics must remain available during a database outage so the outage
        // itself is observable. The public response never includes DB errors.
    }

    const auto pool = pool_.stats();
    metrics.poolMax = pool.max;
    metrics.poolIdle = pool.idle;
    metrics.poolOutstanding = pool.outstanding;
    metrics.poolTimeouts = pool.timeouts;
    return metrics;
}

std::string PostgresStorage::uniqueSlug(pqxx::work& tx, const std::string& title) {
    auto base = slugBase(title);
    // Share slugs are bearer capabilities. Use 128 random bits so their margin
    // remains strong even with many diagrams and long-lived public links.
    std::string slug = base + "-" + randomHex(16);
    int i = 2;
    while (!tx.exec("SELECT 1 FROM diagrams WHERE slug=$1", pqxx::params{slug}).empty()) {
        slug = base + "-" + randomHex(16) + "-" + std::to_string(i++);
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
        SELECT d.id, d.title, d.slug, d.description, d.revision,
               (d.sharing_enabled AND
                (d.share_expires_at IS NULL OR d.share_expires_at > now())) AS sharing_enabled,
               (d.share_password_hash IS NOT NULL) AS share_password_protected,
               (d.sharing_enabled AND d.share_expires_at IS NOT NULL AND
                d.share_expires_at <= now()) AS share_expired,
               to_char(d.share_expires_at AT TIME ZONE 'UTC',
                       'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS share_expires_at,
               to_char(d.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at,
               to_char(d.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS updated_at,
               NULL::text AS deleted_at,
               (SELECT count(*) FROM nodes n WHERE n.diagram_id=d.id) AS node_count,
               (SELECT count(*) FROM edges e WHERE e.diagram_id=d.id) AS edge_count
        FROM diagrams d
        WHERE d.deleted_at IS NULL
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
        SELECT d.id, d.title, d.slug, d.description, d.revision,
               (d.sharing_enabled AND
                (d.share_expires_at IS NULL OR d.share_expires_at > now())) AS sharing_enabled,
               (d.share_password_hash IS NOT NULL) AS share_password_protected,
               (d.sharing_enabled AND d.share_expires_at IS NOT NULL AND
                d.share_expires_at <= now()) AS share_expired,
               to_char(d.share_expires_at AT TIME ZONE 'UTC',
                       'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS share_expires_at,
               to_char(d.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at,
               to_char(d.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS updated_at,
               NULL::text AS deleted_at,
               (SELECT count(*) FROM nodes n WHERE n.diagram_id=d.id) AS node_count,
               (SELECT count(*) FROM edges e WHERE e.diagram_id=d.id) AS edge_count
        FROM diagrams d
        WHERE d.owner_token_hash=$1 AND d.deleted_at IS NULL
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

std::vector<nlohmann::json> PostgresStorage::listDeletedDiagrams(long limit, long offset) {
    auto conn = pool_.acquire();
    pqxx::read_transaction tx(conn.get());
    const auto rows = tx.exec(R"SQL(
        SELECT d.id, d.title, ''::text AS slug, d.description, d.revision,
               false AS sharing_enabled, false AS share_expired,
               (d.share_password_hash IS NOT NULL) AS share_password_protected,
               NULL::text AS share_expires_at,
               to_char(d.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at,
               to_char(d.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS updated_at,
               to_char(d.deleted_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS deleted_at,
               (SELECT count(*) FROM nodes n WHERE n.diagram_id=d.id) AS node_count,
               (SELECT count(*) FROM edges e WHERE e.diagram_id=d.id) AS edge_count
        FROM diagrams d
        WHERE d.deleted_at IS NOT NULL
        ORDER BY d.deleted_at DESC, d.id DESC
        LIMIT $1 OFFSET $2
    )SQL", pqxx::params{limit, offset});
    std::vector<nlohmann::json> out;
    for (const auto& row : rows) {
        out.push_back(diagramSummaryJson(row));
    }
    return out;
}

std::vector<nlohmann::json> PostgresStorage::listDeletedDiagramsForOwner(
    const std::string& ownerTokenHash, long limit, long offset) {
    if (ownerTokenHash.empty()) {
        return {};
    }
    auto conn = pool_.acquire();
    pqxx::read_transaction tx(conn.get());
    const auto rows = tx.exec(R"SQL(
        SELECT d.id, d.title, ''::text AS slug, d.description, d.revision,
               false AS sharing_enabled, false AS share_expired,
               (d.share_password_hash IS NOT NULL) AS share_password_protected,
               NULL::text AS share_expires_at,
               to_char(d.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at,
               to_char(d.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS updated_at,
               to_char(d.deleted_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS deleted_at,
               (SELECT count(*) FROM nodes n WHERE n.diagram_id=d.id) AS node_count,
               (SELECT count(*) FROM edges e WHERE e.diagram_id=d.id) AS edge_count
        FROM diagrams d
        WHERE d.owner_token_hash=$1 AND d.deleted_at IS NOT NULL
        ORDER BY d.deleted_at DESC, d.id DESC
        LIMIT $2 OFFSET $3
    )SQL", pqxx::params{ownerTokenHash, limit, offset});
    std::vector<nlohmann::json> out;
    for (const auto& row : rows) {
        out.push_back(diagramSummaryJson(row));
    }
    return out;
}

Diagram PostgresStorage::getDiagram(long id) {
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    auto diagram = getDiagram(tx, id);
    tx.commit();
    return diagram;
}

Diagram PostgresStorage::getDiagramBySlug(
    const std::string& slug, long authorizedPasswordVersion) {
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    auto rows = tx.exec(
        R"SQL(
            SELECT id FROM diagrams
            WHERE slug=$1 AND sharing_enabled=true
              AND deleted_at IS NULL
              AND (share_expires_at IS NULL OR share_expires_at > now())
              AND (share_password_hash IS NULL OR share_password_version=$2)
        )SQL",
        pqxx::params{slug, authorizedPasswordVersion});
    if (rows.empty()) {
        throw std::runtime_error("diagram not found");
    }
    auto diagram = getDiagram(tx, rows[0]["id"].as<long>());
    tx.commit();
    return diagram;
}

std::optional<ShareAccessPolicy> PostgresStorage::shareAccessPolicy(
    const std::string& slug) {
    auto conn = pool_.acquire();
    pqxx::read_transaction tx(conn.get());
    const auto rows = tx.exec(R"SQL(
        SELECT share_password_hash, share_password_version
        FROM diagrams
        WHERE slug=$1 AND sharing_enabled=true
          AND deleted_at IS NULL
          AND (share_expires_at IS NULL OR share_expires_at > now())
    )SQL", pqxx::params{slug});
    if (rows.empty()) {
        return std::nullopt;
    }
    ShareAccessPolicy policy;
    policy.passwordProtected = !rows[0]["share_password_hash"].is_null();
    policy.passwordVersion = rows[0]["share_password_version"].as<long>();
    policy.passwordHash = rowString(rows[0], "share_password_hash");
    return policy;
}

std::string PostgresStorage::ownerHashForDiagram(long id) {
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    auto rows = tx.exec(
        "SELECT owner_token_hash FROM diagrams WHERE id=$1 AND deleted_at IS NULL",
        pqxx::params{id});
    if (rows.empty()) {
        throw std::runtime_error("diagram not found");
    }
    auto owner = rows[0]["owner_token_hash"].is_null() ? "" : rows[0]["owner_token_hash"].as<std::string>();
    tx.commit();
    return owner;
}

std::string PostgresStorage::ownerHashForDeletedDiagram(long id) {
    auto conn = pool_.acquire();
    pqxx::read_transaction tx(conn.get());
    const auto rows = tx.exec(
        "SELECT owner_token_hash FROM diagrams WHERE id=$1 AND deleted_at IS NOT NULL",
        pqxx::params{id});
    if (rows.empty()) {
        throw std::runtime_error("trash item not found");
    }
    return rows[0]["owner_token_hash"].is_null()
        ? ""
        : rows[0]["owner_token_hash"].as<std::string>();
}

long PostgresStorage::countDiagrams() {
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    auto rows = tx.exec("SELECT count(*) AS n FROM diagrams WHERE deleted_at IS NULL");
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
    auto rows = tx.exec(
        "SELECT count(*) AS n FROM diagrams WHERE owner_token_hash=$1 AND deleted_at IS NULL",
        pqxx::params{ownerTokenHash});
    auto count = rows[0]["n"].as<long>();
    tx.commit();
    return count;
}

long PostgresStorage::countDeletedDiagrams() {
    auto conn = pool_.acquire();
    pqxx::read_transaction tx(conn.get());
    return tx.exec("SELECT count(*) AS n FROM diagrams WHERE deleted_at IS NOT NULL")[0]["n"].as<long>();
}

long PostgresStorage::countDeletedDiagramsForOwner(const std::string& ownerTokenHash) {
    if (ownerTokenHash.empty()) {
        return 0;
    }
    auto conn = pool_.acquire();
    pqxx::read_transaction tx(conn.get());
    return tx.exec(
        "SELECT count(*) AS n FROM diagrams WHERE owner_token_hash=$1 AND deleted_at IS NOT NULL",
        pqxx::params{ownerTokenHash})[0]["n"].as<long>();
}

nlohmann::json PostgresStorage::exportDataForOwner(const std::string& ownerTokenHash) {
    if (ownerTokenHash.empty()) {
        throw std::invalid_argument("guest ownership is required for data export");
    }

    auto conn = pool_.acquire();
    pqxx::read_transaction tx(conn.get());
    const auto exportedAt = tx.exec(R"SQL(
        SELECT to_char(statement_timestamp() AT TIME ZONE 'UTC',
                       'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS exported_at
    )SQL")[0]["exported_at"].as<std::string>();
    const auto rows = tx.exec(R"SQL(
        SELECT d.id, d.title, d.slug, d.description, d.revision,
               (d.deleted_at IS NULL AND d.sharing_enabled AND
                (d.share_expires_at IS NULL OR d.share_expires_at > statement_timestamp()))
                   AS sharing_active,
               (d.share_password_hash IS NOT NULL) AS share_password_protected,
               (d.sharing_enabled AND d.share_expires_at IS NOT NULL AND
                d.share_expires_at <= statement_timestamp()) AS share_expired,
               to_char(d.share_expires_at AT TIME ZONE 'UTC',
                       'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS share_expires_at,
               to_char(d.created_at AT TIME ZONE 'UTC',
                       'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at,
               to_char(d.updated_at AT TIME ZONE 'UTC',
                       'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS updated_at,
               to_char(d.deleted_at AT TIME ZONE 'UTC',
                       'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS deleted_at,
               COALESCE((
                   SELECT jsonb_agg(jsonb_build_object(
                       'key', n.node_key, 'type', n.type, 'title', n.title,
                       'x', n.x, 'y', n.y, 'width', n.width, 'height', n.height,
                       'color', n.color, 'metadata', n.metadata_json
                   ) ORDER BY n.id)
                   FROM nodes n WHERE n.diagram_id=d.id
               ), '[]'::jsonb)::text AS nodes_json,
               COALESCE((
                   SELECT jsonb_agg(jsonb_build_object(
                       'key', e.edge_key, 'source', e.source_node_key,
                       'target', e.target_node_key, 'label', e.label,
                       'directed', e.directed, 'color', e.color,
                       'metadata', e.metadata_json
                   ) ORDER BY e.id)
                   FROM edges e WHERE e.diagram_id=d.id
               ), '[]'::jsonb)::text AS edges_json,
               COALESCE((
                   SELECT jsonb_agg(jsonb_build_object(
                       'id', v.id, 'version_number', v.version_number,
                       'created_at', to_char(v.created_at AT TIME ZONE 'UTC',
                           'YYYY-MM-DD"T"HH24:MI:SS"Z"'),
                       'note', v.note, 'snapshot', v.snapshot_json
                   ) ORDER BY v.version_number ASC, v.id ASC)
                   FROM diagram_versions v WHERE v.diagram_id=d.id
               ), '[]'::jsonb)::text AS versions_json
        FROM diagrams d
        WHERE d.owner_token_hash=$1
        ORDER BY d.created_at ASC, d.id ASC
    )SQL", pqxx::params{ownerTokenHash});

    nlohmann::json diagrams = nlohmann::json::array();
    for (const auto& row : rows) {
        const bool sharingActive = row["sharing_active"].as<bool>();
        const auto deletedAt = rowString(row, "deleted_at");
        nlohmann::json diagram = {
            {"id", row["id"].as<long>()},
            {"title", rowString(row, "title")},
            {"slug", sharingActive ? rowString(row, "slug") : ""},
            {"description", rowString(row, "description")},
            {"revision", row["revision"].as<long>()},
            {"sharing_enabled", sharingActive},
            {"share_password_protected", row["share_password_protected"].as<bool>()},
            {"share_expired", row["share_expired"].as<bool>()},
            {"created_at", rowString(row, "created_at")},
            {"updated_at", rowString(row, "updated_at")},
            {"nodes", nlohmann::json::parse(rowString(row, "nodes_json"))},
            {"edges", nlohmann::json::parse(rowString(row, "edges_json"))}
        };
        diagram["share_expires_at"] = row["share_expires_at"].is_null()
            ? nlohmann::json(nullptr)
            : nlohmann::json(rowString(row, "share_expires_at"));

        auto versions = nlohmann::json::parse(rowString(row, "versions_json"));
        for (auto& version : versions) {
            if (!version.contains("snapshot") || !version["snapshot"].is_object()) continue;
            auto& snapshot = version["snapshot"];
            // Historical share slugs are security capabilities, not portable
            // content. Exclude them even after they have been revoked.
            snapshot["slug"] = "";
            snapshot["sharing_enabled"] = false;
            snapshot["share_password_protected"] = false;
            snapshot["share_expires_at"] = nullptr;
            snapshot.erase("share_url");
            snapshot.erase("can_edit");
        }

        diagrams.push_back({
            {"state", deletedAt.empty() ? "active" : "trash"},
            {"deleted_at", deletedAt.empty() ? nlohmann::json(nullptr)
                                               : nlohmann::json(deletedAt)},
            {"diagram", std::move(diagram)},
            {"versions", std::move(versions)}
        });
    }

    return {
        {"format", "nuigraph-owner-export-v1"},
        {"exported_at", exportedAt},
        {"scope", "guest_session"},
        {"diagrams", std::move(diagrams)}
    };
}

std::uint64_t PostgresStorage::deleteAllDataForOwner(const std::string& ownerTokenHash) {
    if (ownerTokenHash.empty()) {
        throw std::invalid_argument("guest ownership is required for data deletion");
    }

    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    // Match createDiagram's global-then-owner order. Whichever transaction gets
    // these locks first becomes the linearization point for a racing create.
    tx.exec("SELECT pg_advisory_xact_lock(hashtextextended('nuigraph:global-diagram-quota:v1', 0))");
    tx.exec("SELECT pg_advisory_xact_lock(hashtextextended($1, 0))",
            pqxx::params{ownerTokenHash});
    const auto deleted = tx.exec(R"SQL(
        WITH removed AS (
            DELETE FROM diagrams
            WHERE owner_token_hash=$1
            RETURNING id
        )
        SELECT count(*) AS n FROM removed
    )SQL", pqxx::params{ownerTokenHash})[0]["n"].as<std::uint64_t>();
    tx.commit();
    return deleted;
}

bool PostgresStorage::consumeRateLimit(const std::string& bucketKey,
                                       std::size_t maxRequests,
                                       long long windowSeconds) {
    if (bucketKey.size() != 64 || maxRequests == 0 || windowSeconds <= 0) {
        throw std::invalid_argument("invalid persistent rate-limit parameters");
    }

    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());

    // Only new identities take this advisory lock. It serialises bounded-table
    // admission across app instances without blocking updates to existing
    // buckets. Expired rows are removed before capacity is evaluated so a
    // rotating-address flood cannot grow storage indefinitely.
    auto known = tx.exec("SELECT 1 FROM rate_limit_buckets WHERE bucket_key=$1",
                         pqxx::params{bucketKey});
    if (known.empty()) {
        tx.exec("SELECT pg_advisory_xact_lock("
                "hashtextextended('nuigraph:rate-limit-admission:v1', 0))");
        tx.exec("DELETE FROM rate_limit_buckets WHERE expires_at <= statement_timestamp()");
        known = tx.exec("SELECT 1 FROM rate_limit_buckets WHERE bucket_key=$1",
                        pqxx::params{bucketKey});
        if (known.empty()) {
            const auto tracked = tx.exec(
                "SELECT count(*) AS n FROM rate_limit_buckets")[0]["n"].as<long>();
            if (tracked >= kMaxPersistentRateLimitKeys) {
                tx.commit();
                return false;
            }
        }
    }

    // Generic Cell Rate Algorithm (GCRA): the row stores the theoretical next
    // arrival. PostgreSQL's statement clock and ON CONFLICT row lock make one
    // decision atomic across threads, processes and hosts. Rejected attempts do
    // not extend the lockout; capacity refills smoothly over the configured
    // window rather than doubling at a fixed-window boundary.
    const double intervalSeconds =
        static_cast<double>(windowSeconds) / static_cast<double>(maxRequests);
    const double burstSeconds = intervalSeconds * static_cast<double>(maxRequests - 1);
    auto accepted = tx.exec(R"SQL(
        INSERT INTO rate_limit_buckets
            (bucket_key, theoretical_arrival_at, expires_at)
        VALUES
            ($1,
             statement_timestamp() + ($2 * interval '1 second'),
             statement_timestamp() + ($3 * interval '1 second'))
        ON CONFLICT (bucket_key) DO UPDATE
        SET theoretical_arrival_at =
                GREATEST(statement_timestamp(), rate_limit_buckets.theoretical_arrival_at)
                + ($2 * interval '1 second'),
            expires_at = statement_timestamp() + ($3 * interval '1 second')
        WHERE statement_timestamp() >=
              rate_limit_buckets.theoretical_arrival_at
              - ($4 * interval '1 second')
        RETURNING bucket_key
    )SQL", pqxx::params{bucketKey, intervalSeconds, windowSeconds, burstSeconds});
    tx.commit();
    return !accepted.empty();
}

void PostgresStorage::clearRateLimit(const std::string& bucketKey) {
    if (bucketKey.size() != 64) {
        throw std::invalid_argument("invalid persistent rate-limit key");
    }
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    tx.exec("DELETE FROM rate_limit_buckets WHERE bucket_key=$1",
            pqxx::params{bucketKey});
    tx.commit();
}

Diagram PostgresStorage::getDiagram(pqxx::work& tx, long id) {
    auto rows = tx.exec(R"SQL(
        SELECT id, title, slug, description, revision,
               (sharing_enabled AND
                (share_expires_at IS NULL OR share_expires_at > now())) AS sharing_enabled,
               (share_password_hash IS NOT NULL) AS share_password_protected,
               (sharing_enabled AND share_expires_at IS NOT NULL AND
                share_expires_at <= now()) AS share_expired,
               to_char(share_expires_at AT TIME ZONE 'UTC',
                       'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS share_expires_at,
               to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at,
               to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS updated_at
        FROM diagrams WHERE id=$1 AND deleted_at IS NULL
        FOR SHARE
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
    diagram.revision = d["revision"].as<long>();
    diagram.sharingEnabled = d["sharing_enabled"].as<bool>();
    diagram.shareExpiresAt = rowString(d, "share_expires_at");
    diagram.shareExpired = d["share_expired"].as<bool>();
    diagram.sharePasswordProtected = d["share_password_protected"].as<bool>();

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

void PostgresStorage::enforceLogicalStorageCapacity(
    pqxx::work& tx, const std::string& ownerTokenHash) {
    const auto total = tx.exec(R"SQL(
        SELECT logical_bytes
        FROM public.logical_storage_usage
        WHERE singleton=true
    )SQL");
    if (total.empty()) {
        throw std::runtime_error("logical storage usage is unavailable");
    }
    if (total[0]["logical_bytes"].as<std::uint64_t>() > cfg_.maxLogicalBytesTotal) {
        throw DiagramCapacityExceeded(
            "logical storage capacity is temporarily exhausted; permanently delete trash before retrying");
    }

    if (ownerTokenHash.empty()) {
        return;
    }
    const auto owned = tx.exec(R"SQL(
        SELECT
            COALESCE((
                SELECT sum(d.logical_bytes)
                FROM diagrams d
                WHERE d.owner_token_hash=$1
            ), 0)
            + COALESCE((
                SELECT sum(v.logical_bytes)
                FROM diagram_versions v
                JOIN diagrams d ON d.id=v.diagram_id
                WHERE d.owner_token_hash=$1
            ), 0) AS logical_bytes
    )SQL", pqxx::params{ownerTokenHash});
    if (owned[0]["logical_bytes"].as<std::uint64_t>() > cfg_.maxLogicalBytesPerGuest) {
        throw DiagramQuotaExceeded(
            "logical storage limit reached for this session; permanently delete trash before retrying");
    }
}

DiagramCreationResult PostgresStorage::createDiagram(
    const Diagram& input, bool importMode, const std::string& ownerTokenHash,
    const std::optional<IdempotencyRequest>& idempotency) {
    Diagram diagram = input;
    auto validation = validator_.validate(diagram, importMode);
    if (!validation.ok) {
        throw std::runtime_error(validation.errors.front());
    }
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());

    if (idempotency.has_value()) {
        const auto& request = *idempotency;
        if (!isLowerHex(request.principalKey, 64) ||
            std::find(kIdempotentCreateOperations.begin(), kIdempotentCreateOperations.end(),
                      request.operation) == kIdempotentCreateOperations.end() ||
            !isLowerHex(request.keyHash, 64) || !isLowerHex(request.requestHash, 64)) {
            throw std::invalid_argument("invalid idempotency parameters");
        }

        const auto lockKey = "nuigraph:idempotency:v1:" + request.principalKey + ":" +
            request.operation + ":" + request.keyHash;
        tx.exec("SELECT pg_advisory_xact_lock(hashtextextended($1, 0))",
                pqxx::params{lockKey});
        // Expire only this lookup. A request must not turn a hot creation path
        // into an unbounded cleanup job or contend on unrelated principals.
        tx.exec(R"SQL(
            DELETE FROM diagram_idempotency_keys
            WHERE principal_key=$1 AND operation=$2 AND key_hash=$3
              AND expires_at <= statement_timestamp()
        )SQL", pqxx::params{request.principalKey, request.operation, request.keyHash});
        const auto existing = tx.exec(R"SQL(
            SELECT k.request_hash, k.diagram_id, (d.deleted_at IS NULL) AS result_active
            FROM diagram_idempotency_keys k
            JOIN diagrams d ON d.id=k.diagram_id
            WHERE k.principal_key=$1 AND k.operation=$2 AND k.key_hash=$3
        )SQL", pqxx::params{request.principalKey, request.operation, request.keyHash});
        if (!existing.empty()) {
            if (rowString(existing[0], "request_hash") != request.requestHash) {
                throw IdempotencyConflict(
                    "idempotency key was already used for a different request");
            }
            if (!existing[0]["result_active"].as<bool>()) {
                throw IdempotencyConflict(
                    "the idempotent result is in trash; restore it or use a new key");
            }
            Diagram replay;
            try {
                replay = getDiagram(tx, existing[0]["diagram_id"].as<long>());
            } catch (const std::runtime_error& e) {
                if (std::string(e.what()) != "diagram not found") {
                    throw;
                }
                // Trash/permanent delete can race this replay. The advisory
                // key lock serializes creators, while deliberately avoiding a
                // ledger-then-diagram row-lock order that could deadlock with
                // ON DELETE CASCADE's diagram-then-ledger order.
                throw IdempotencyConflict(
                    "the idempotent result is no longer active; restore it or use a new key");
            }
            tx.commit();
            return {std::move(replay), true};
        }
    }

    // Every creation path reaches this method. Serialise the global count and
    // insert in one transaction so parallel requests and fresh guest sessions
    // cannot race past the retained-data ceiling. This lock is always acquired
    // before the per-owner lock below, giving concurrent creators one fixed lock
    // order and avoiding a quota-lock deadlock.
    tx.exec("SELECT pg_advisory_xact_lock(hashtextextended('nuigraph:global-diagram-quota:v1', 0))");
    const auto total = tx.exec("SELECT count(*) AS n FROM diagrams")[0]["n"].as<long>();
    if (total >= cfg_.maxDiagramsTotal) {
        throw DiagramCapacityExceeded(
            "diagram capacity is temporarily exhausted; permanently delete trash before retrying");
    }

    if (!ownerTokenHash.empty()) {
        // Count-and-insert must be one serial operation per owner. The old route
        // pre-check ran in a different transaction, so parallel requests could
        // all observe one remaining slot and cross the configured ceiling.
        // A transaction-scoped advisory lock avoids a durable lock table and is
        // released automatically on commit or rollback.
        tx.exec("SELECT pg_advisory_xact_lock(hashtextextended($1, 0))",
                pqxx::params{ownerTokenHash});
        const auto owned = tx.exec(
            "SELECT count(*) AS n FROM diagrams WHERE owner_token_hash=$1",
            pqxx::params{ownerTokenHash})[0]["n"].as<long>();
        if (owned >= cfg_.maxDiagramsPerGuest) {
            throw DiagramQuotaExceeded(
                "diagram limit reached for this session; permanently delete trash to make room");
        }
    }

    auto slug = uniqueSlug(tx, diagram.title);
    auto rows = tx.exec(R"SQL(
        INSERT INTO diagrams (title, slug, description, owner_token_hash, logical_bytes)
        VALUES ($1,$2,$3,$4,$5)
        RETURNING id
    )SQL", pqxx::params{
        diagram.title, slug, diagram.description, ownerTokenHash,
        static_cast<long long>(logicalDiagramBytes(diagram))});
    long id = rows[0]["id"].as<long>();
    replaceNodesAndEdges(tx, id, diagram);
    createVersion(tx, id, "initial snapshot");
    enforceLogicalStorageCapacity(tx, ownerTokenHash);
    if (idempotency.has_value()) {
        const auto& request = *idempotency;
        tx.exec(R"SQL(
            INSERT INTO diagram_idempotency_keys
                (principal_key, operation, key_hash, request_hash, diagram_id)
            VALUES ($1,$2,$3,$4,$5)
        )SQL", pqxx::params{
            request.principalKey, request.operation, request.keyHash, request.requestHash, id});
    }
    // Re-read through the transaction's existing lease. Under a burst equal to
    // DB_POOL_SIZE, acquiring a second connection here would deadlock the
    // winning request against contenders waiting on its advisory lock.
    auto created = getDiagram(tx, id);
    tx.commit();
    return {std::move(created), false};
}

Diagram PostgresStorage::updateDiagram(long id, const Diagram& input, const std::string& note,
                                       bool importMode, long expectedRevision) {
    Diagram diagram = input;
    diagram.id = id;
    auto validation = validator_.validate(diagram, importMode);
    if (!validation.ok) {
        throw std::runtime_error(validation.errors.front());
    }
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    auto updated = tx.exec(R"SQL(
        UPDATE diagrams
        SET title=$1, description=$2, logical_bytes=$3,
            updated_at=now(), revision=revision+1
        WHERE id=$4 AND revision=$5 AND deleted_at IS NULL
        RETURNING revision, owner_token_hash
    )SQL", pqxx::params{
        diagram.title, diagram.description,
        static_cast<long long>(logicalDiagramBytes(diagram)), id, expectedRevision});
    if (updated.empty()) {
        auto current = tx.exec(
            "SELECT revision FROM diagrams WHERE id=$1 AND deleted_at IS NULL",
            pqxx::params{id});
        if (current.empty()) {
            throw std::runtime_error("diagram not found");
        }
        throw DiagramRevisionConflict(
            current[0]["revision"].as<long>(),
            "diagram changed since it was opened; reload before saving");
    }
    replaceNodesAndEdges(tx, id, diagram);
    createVersion(tx, id, note.empty() ? "saved snapshot" : note);
    enforceLogicalStorageCapacity(tx, rowString(updated[0], "owner_token_hash"));
    tx.commit();
    return getDiagram(id);
}

Diagram PostgresStorage::updateSharing(long id, ShareAction action, long expectedRevision,
                                       const ShareExpiry& expiry) {
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    const auto rows = tx.exec(R"SQL(
        SELECT title, sharing_enabled,
               (sharing_enabled AND
                (share_expires_at IS NULL OR share_expires_at > now())) AS sharing_active,
               revision
        FROM diagrams
        WHERE id=$1 AND deleted_at IS NULL
        FOR UPDATE
    )SQL", pqxx::params{id});
    if (rows.empty()) {
        throw std::runtime_error("diagram not found");
    }

    const auto currentRevision = rows[0]["revision"].as<long>();
    if (currentRevision != expectedRevision) {
        throw DiagramRevisionConflict(
            currentRevision,
            "diagram changed since it was opened; reload before changing sharing");
    }
    const auto storedEnabled = rows[0]["sharing_enabled"].as<bool>();
    const auto currentlyActive = rows[0]["sharing_active"].as<bool>();
    if (expiry.seconds.has_value() && *expiry.seconds <= 0) {
        throw std::invalid_argument("share expiry must be positive");
    }

    if (action == ShareAction::Disable) {
        if (!storedEnabled) {
            throw std::invalid_argument("sharing is already disabled");
        }
        tx.exec(R"SQL(
            UPDATE diagrams
            SET sharing_enabled=false, share_expires_at=NULL,
                share_password_hash=NULL,
                share_password_version=share_password_version+1,
                updated_at=now(), revision=revision+1
            WHERE id=$1
        )SQL", pqxx::params{id});
    } else if (action == ShareAction::SetExpiry) {
        if (!currentlyActive) {
            throw std::invalid_argument("cannot change expiry for an inactive share link");
        }
        if (expiry.preserve) {
            throw std::invalid_argument("share expiry update is missing a value");
        }
        if (expiry.seconds.has_value()) {
            tx.exec(R"SQL(
                UPDATE diagrams
                SET share_expires_at=now() + ($1 * interval '1 second'),
                    updated_at=now(), revision=revision+1
                WHERE id=$2
            )SQL", pqxx::params{*expiry.seconds, id});
        } else {
            tx.exec(R"SQL(
                UPDATE diagrams
                SET share_expires_at=NULL, updated_at=now(), revision=revision+1
                WHERE id=$1
            )SQL", pqxx::params{id});
        }
    } else {
        if (action == ShareAction::Enable && currentlyActive) {
            throw std::invalid_argument("sharing is already enabled");
        }
        if (action == ShareAction::Rotate && !currentlyActive) {
            throw std::invalid_argument("cannot rotate an inactive share link");
        }
        // Enabling also rotates. A URL copied before revocation must never
        // become valid again merely because sharing was re-enabled later. The
        // same rule applies when an expired link is enabled again.
        const auto slug = uniqueSlug(tx, rowString(rows[0], "title"));
        if (expiry.preserve) {
            tx.exec(R"SQL(
                UPDATE diagrams
                SET slug=$1, sharing_enabled=true, updated_at=now(), revision=revision+1
                WHERE id=$2
            )SQL", pqxx::params{slug, id});
        } else if (expiry.seconds.has_value()) {
            tx.exec(R"SQL(
                UPDATE diagrams
                SET slug=$1, sharing_enabled=true,
                    share_expires_at=now() + ($2 * interval '1 second'),
                    updated_at=now(), revision=revision+1
                WHERE id=$3
            )SQL", pqxx::params{slug, *expiry.seconds, id});
        } else {
            tx.exec(R"SQL(
                UPDATE diagrams
                SET slug=$1, sharing_enabled=true, share_expires_at=NULL,
                    updated_at=now(), revision=revision+1
                WHERE id=$2
            )SQL", pqxx::params{slug, id});
        }
    }

    tx.commit();
    return getDiagram(id);
}

Diagram PostgresStorage::updateSharePassword(
    long id, long expectedRevision,
    const std::optional<std::string>& passwordHash) {
    if (passwordHash.has_value()) {
        const auto& encoded = *passwordHash;
        if (encoded.size() > 256 ||
            !encoded.starts_with("pbkdf2_sha256$210000$")) {
            throw std::invalid_argument("invalid share password verifier");
        }
    }

    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    const auto rows = tx.exec(R"SQL(
        SELECT revision, sharing_enabled,
               (share_expires_at IS NULL OR share_expires_at > now()) AS not_expired,
               (share_password_hash IS NOT NULL) AS password_protected
        FROM diagrams
        WHERE id=$1 AND deleted_at IS NULL
        FOR UPDATE
    )SQL", pqxx::params{id});
    if (rows.empty()) {
        throw std::runtime_error("diagram not found");
    }
    const auto currentRevision = rows[0]["revision"].as<long>();
    if (currentRevision != expectedRevision) {
        throw DiagramRevisionConflict(
            currentRevision,
            "diagram changed since it was opened; reload before changing share protection");
    }
    if (!rows[0]["sharing_enabled"].as<bool>() ||
        !rows[0]["not_expired"].as<bool>()) {
        throw std::invalid_argument("cannot change password for an inactive share link");
    }
    const auto currentlyProtected = rows[0]["password_protected"].as<bool>();
    if (!passwordHash.has_value() && !currentlyProtected) {
        throw std::invalid_argument("share link is not password protected");
    }

    if (passwordHash.has_value()) {
        tx.exec(R"SQL(
            UPDATE diagrams
            SET share_password_hash=$1,
                share_password_version=share_password_version+1,
                updated_at=statement_timestamp(), revision=revision+1
            WHERE id=$2
        )SQL", pqxx::params{*passwordHash, id});
    } else {
        tx.exec(R"SQL(
            UPDATE diagrams
            SET share_password_hash=NULL,
                share_password_version=share_password_version+1,
                updated_at=statement_timestamp(), revision=revision+1
            WHERE id=$1
        )SQL", pqxx::params{id});
    }
    tx.commit();
    return getDiagram(id);
}

void PostgresStorage::trashDiagram(long id, long expectedRevision) {
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    const auto moved = tx.exec(R"SQL(
        UPDATE diagrams
        SET deleted_at=statement_timestamp(), sharing_enabled=false,
            share_expires_at=NULL, share_password_hash=NULL,
            share_password_version=share_password_version+1,
            updated_at=statement_timestamp(),
            revision=revision+1
        WHERE id=$1 AND revision=$2 AND deleted_at IS NULL
        RETURNING id
    )SQL",
        pqxx::params{id, expectedRevision});
    if (moved.empty()) {
        const auto current = tx.exec(
            "SELECT revision FROM diagrams WHERE id=$1 AND deleted_at IS NULL",
            pqxx::params{id});
        if (current.empty()) {
            throw std::runtime_error("diagram not found");
        }
        throw DiagramRevisionConflict(
            current[0]["revision"].as<long>(),
            "diagram changed since it was opened; reload before moving it to trash");
    }
    tx.commit();
}

Diagram PostgresStorage::restoreDiagramFromTrash(long id, long expectedRevision) {
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    const auto restored = tx.exec(R"SQL(
        UPDATE diagrams
        SET deleted_at=NULL, sharing_enabled=false, share_expires_at=NULL,
            updated_at=statement_timestamp(), revision=revision+1
        WHERE id=$1 AND revision=$2 AND deleted_at IS NOT NULL
        RETURNING id
    )SQL", pqxx::params{id, expectedRevision});
    if (restored.empty()) {
        const auto current = tx.exec(
            "SELECT revision FROM diagrams WHERE id=$1 AND deleted_at IS NOT NULL",
            pqxx::params{id});
        if (current.empty()) {
            throw std::runtime_error("trash item not found");
        }
        throw DiagramRevisionConflict(
            current[0]["revision"].as<long>(),
            "trash item changed since it was listed; refresh trash before restoring");
    }
    auto diagram = getDiagram(tx, id);
    tx.commit();
    return diagram;
}

void PostgresStorage::permanentlyDeleteDiagram(long id, long expectedRevision) {
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    const auto deleted = tx.exec(R"SQL(
        DELETE FROM diagrams
        WHERE id=$1 AND revision=$2 AND deleted_at IS NOT NULL
        RETURNING id
    )SQL", pqxx::params{id, expectedRevision});
    if (deleted.empty()) {
        const auto current = tx.exec(
            "SELECT revision FROM diagrams WHERE id=$1 AND deleted_at IS NOT NULL",
            pqxx::params{id});
        if (current.empty()) {
            throw std::runtime_error("trash item not found");
        }
        throw DiagramRevisionConflict(
            current[0]["revision"].as<long>(),
            "trash item changed since it was listed; refresh trash before permanent deletion");
    }
    tx.commit();
}

TrashPurgeResult PostgresStorage::purgeExpiredTrash(
    int retentionDays, int batchSize, int intervalSeconds) {
    if (retentionDays < 1 || batchSize < 1 || batchSize > 500 ||
        intervalSeconds < 60 || intervalSeconds > 86400) {
        throw std::invalid_argument("invalid trash retention policy");
    }

    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    const auto claimed = tx.exec(R"SQL(
        WITH due AS (
            SELECT job_name
            FROM public.maintenance_jobs
            WHERE job_name='trash_purge'
              AND next_run_at <= statement_timestamp()
            FOR UPDATE SKIP LOCKED
        )
        UPDATE public.maintenance_jobs j
        SET next_run_at = statement_timestamp() + ($1 * interval '1 second')
        FROM due
        WHERE j.job_name=due.job_name
        RETURNING j.job_name
    )SQL", pqxx::params{intervalSeconds});
    if (claimed.empty()) {
        tx.commit();
        return {};
    }

    const auto deleted = tx.exec(R"SQL(
        WITH expired AS (
            SELECT id
            FROM public.diagrams
            WHERE deleted_at IS NOT NULL
              AND deleted_at <= statement_timestamp() - ($1 * interval '1 day')
            ORDER BY deleted_at ASC, id ASC
            FOR UPDATE SKIP LOCKED
            LIMIT $2
        ), removed AS (
            DELETE FROM public.diagrams d
            USING expired e
            WHERE d.id=e.id
            RETURNING d.id
        )
        SELECT count(*) AS n FROM removed
    )SQL", pqxx::params{retentionDays, batchSize})[0]["n"].as<std::uint64_t>();

    tx.exec(R"SQL(
        UPDATE public.maintenance_jobs
        SET last_success_at=statement_timestamp(), last_deleted_count=$1
        WHERE job_name='trash_purge'
    )SQL", pqxx::params{static_cast<long long>(deleted)});
    tx.commit();
    return {true, deleted};
}

DiagramCreationResult PostgresStorage::duplicateDiagram(
    long id, const std::string& ownerTokenHash,
    const std::optional<IdempotencyRequest>& idempotency) {
    auto source = getDiagram(id);
    source.id = 0;
    source.title += " Copy";
    source.slug.clear();
    return createDiagram(source, false, ownerTokenHash, idempotency);
}

DiagramCreationResult PostgresStorage::forkDiagramBySlug(
    const std::string& slug, const std::string& ownerTokenHash,
    const std::optional<IdempotencyRequest>& idempotency,
    long authorizedPasswordVersion) {
    auto source = getDiagramBySlug(slug, authorizedPasswordVersion);
    source.id = 0;
    source.title += " Copy";
    source.slug.clear();
    source.sharingEnabled = false;
    return createDiagram(source, false, ownerTokenHash, idempotency);
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
          AND EXISTS (
              SELECT 1 FROM diagrams d
              WHERE d.id=$1 AND d.deleted_at IS NULL
          )
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
    const auto owner = tx.exec(
        "SELECT owner_token_hash FROM diagrams WHERE id=$1 AND deleted_at IS NULL",
        pqxx::params{diagramId});
    if (owner.empty()) {
        throw std::runtime_error("diagram not found");
    }
    enforceLogicalStorageCapacity(tx, rowString(owner[0], "owner_token_hash"));
    tx.commit();
    return info;
}

VersionInfo PostgresStorage::createVersion(pqxx::work& tx, long diagramId, const std::string& note) {
    if (note.size() > 240) {
        throw std::invalid_argument("version note is too long");
    }

    // MAX(version_number)+1 is safe only while one transaction at a time may
    // assign a version for this diagram. Locking its parent row also serializes
    // saves/restores with manual snapshots and turns the unique constraint into
    // a final invariant rather than normal concurrency control.
    auto locked = tx.exec(
        "SELECT 1 FROM diagrams WHERE id=$1 AND deleted_at IS NULL FOR UPDATE",
        pqxx::params{diagramId});
    if (locked.empty()) {
        throw std::runtime_error("diagram not found");
    }

    auto diagram = getDiagram(tx, diagramId);
    auto next = tx.exec("SELECT COALESCE(MAX(version_number),0)+1 AS n FROM diagram_versions WHERE diagram_id=$1", pqxx::params{diagramId})[0]["n"].as<int>();
    const auto snapshot = diagramToJson(diagram).dump();
    const auto logicalBytes = std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(snapshot.size() + note.size()));
    auto rows = tx.exec(R"SQL(
        INSERT INTO diagram_versions
            (diagram_id, version_number, snapshot_json, note, logical_bytes)
        VALUES ($1,$2,$3::jsonb,$4,$5)
        RETURNING id, diagram_id, version_number,
                  to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"') AS created_at,
                  note
    )SQL", pqxx::params{
        diagramId, next, snapshot, note, static_cast<long long>(logicalBytes)});
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

// Reads a snapshot without writing it back, so a version can be compared with
// the live diagram instead of having to be restored to be looked at. The
// diagram id is part of the WHERE clause, not just the version id, so a caller
// that has been allowed to read one diagram cannot reach another one's history
// by guessing version numbers.
Diagram PostgresStorage::getVersion(long diagramId, long versionId) {
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    auto rows = tx.exec(R"SQL(
        SELECT v.snapshot_json::text
        FROM diagram_versions v
        JOIN diagrams d ON d.id=v.diagram_id
        WHERE v.diagram_id=$1 AND v.id=$2 AND d.deleted_at IS NULL
    )SQL", pqxx::params{diagramId, versionId});
    if (rows.empty()) {
        throw std::runtime_error("version not found");
    }
    Diagram diagram = diagramFromJson(nlohmann::json::parse(rows[0][0].as<std::string>()));
    diagram.id = diagramId;
    // Snapshots predate any later tightening of the rules, so they are clamped
    // on the way out the same as on the way in rather than trusted wholesale.
    auto validation = validator_.validate(diagram, true);
    if (!validation.ok) {
        throw std::runtime_error(validation.errors.front());
    }
    return diagram;
}

Diagram PostgresStorage::restoreVersion(long diagramId, long versionId, long expectedRevision) {
    auto conn = pool_.acquire();
    pqxx::work tx(conn.get());
    auto rows = tx.exec(R"SQL(
        SELECT v.snapshot_json::text
        FROM diagram_versions v
        JOIN diagrams d ON d.id=v.diagram_id
        WHERE v.diagram_id=$1 AND v.id=$2 AND d.deleted_at IS NULL
    )SQL", pqxx::params{diagramId, versionId});
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
    auto updated = tx.exec(R"SQL(
        UPDATE diagrams
        SET title=$1, description=$2, logical_bytes=$3,
            updated_at=now(), revision=revision+1
        WHERE id=$4 AND revision=$5 AND deleted_at IS NULL
        RETURNING revision, owner_token_hash
    )SQL", pqxx::params{
        diagram.title, diagram.description,
        static_cast<long long>(logicalDiagramBytes(diagram)), diagramId, expectedRevision});
    if (updated.empty()) {
        auto current = tx.exec(
            "SELECT revision FROM diagrams WHERE id=$1 AND deleted_at IS NULL",
            pqxx::params{diagramId});
        if (current.empty()) {
            throw std::runtime_error("diagram not found");
        }
        throw DiagramRevisionConflict(
            current[0]["revision"].as<long>(),
            "diagram changed since it was opened; reload before restoring");
    }
    replaceNodesAndEdges(tx, diagramId, diagram);
    createVersion(tx, diagramId, "restored version " + std::to_string(versionId));
    enforceLogicalStorageCapacity(tx, rowString(updated[0], "owner_token_hash"));
    tx.commit();
    return getDiagram(diagramId);
}

} // namespace nuigraph
