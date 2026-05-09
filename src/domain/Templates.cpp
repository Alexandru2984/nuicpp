#include "domain/Templates.hpp"

#include <algorithm>

namespace nuigraph {
namespace {

Diagram makeCloudArchitecture() {
    Diagram d;
    d.title = "Cloud Architecture";
    d.description = "API, services, queue, and PostgreSQL reference map.";
    d.nodes = {
        {"client", "external", "Client Apps", 40, 130, 170, 80, "#fb7185", {}},
        {"gateway", "api", "API Gateway", 300, 130, 180, 80, "#60a5fa", {}},
        {"auth", "service", "Auth Service", 570, 40, 180, 80, "#22c55e", {}},
        {"orders", "service", "Orders Service", 570, 170, 180, 80, "#22c55e", {}},
        {"queue", "process", "Event Queue", 830, 170, 170, 80, "#38bdf8", {}},
        {"db", "database", "PostgreSQL", 830, 40, 170, 90, "#a78bfa", {}},
        {"decision", "decision", "Fraud Check", 1090, 150, 160, 110, "#f59e0b", {}},
        {"note", "note", "Versioned diagrams stored in PostgreSQL", 1030, 20, 260, 90, "#fde047", {}}
    };
    d.edges = {
        {"e_client_gateway", "client", "gateway", "HTTPS", true, "#94a3b8", {}},
        {"e_gateway_auth", "gateway", "auth", "OIDC", true, "#94a3b8", {}},
        {"e_gateway_orders", "gateway", "orders", "REST", true, "#94a3b8", {}},
        {"e_auth_db", "auth", "db", "sessions", true, "#a78bfa", {}},
        {"e_orders_db", "orders", "db", "writes", true, "#a78bfa", {}},
        {"e_orders_queue", "orders", "queue", "events", true, "#38bdf8", {}},
        {"e_queue_decision", "queue", "decision", "async", true, "#f59e0b", {}}
    };
    return d;
}

Diagram makeIncidentFlow() {
    Diagram d;
    d.title = "Incident Response Flow";
    d.description = "Triage, decision, mitigation, communication, and postmortem flowchart.";
    d.nodes = {
        {"alert", "external", "Alert Triggered", 60, 160, 170, 78, "#fb7185", {}},
        {"triage", "process", "Triage", 310, 160, 170, 78, "#38bdf8", {}},
        {"sev", "decision", "Sev 1?", 560, 145, 150, 108, "#f59e0b", {}},
        {"warroom", "service", "War Room", 800, 70, 170, 78, "#22c55e", {}},
        {"mitigate", "process", "Mitigate", 800, 230, 170, 78, "#38bdf8", {}},
        {"notify", "api", "Status Update", 1040, 70, 180, 78, "#60a5fa", {}},
        {"postmortem", "note", "Postmortem within 48h", 1040, 230, 210, 86, "#fde047", {}}
    };
    d.edges = {
        {"e_alert_triage", "alert", "triage", "page", true, "#94a3b8", {}},
        {"e_triage_sev", "triage", "sev", "classify", true, "#94a3b8", {}},
        {"e_sev_warroom", "sev", "warroom", "yes", true, "#f59e0b", {}},
        {"e_sev_mitigate", "sev", "mitigate", "no", true, "#94a3b8", {}},
        {"e_warroom_notify", "warroom", "notify", "updates", true, "#60a5fa", {}},
        {"e_mitigate_postmortem", "mitigate", "postmortem", "learn", true, "#fde047", {}}
    };
    return d;
}

Diagram makeDataPipeline() {
    Diagram d;
    d.title = "Data Pipeline";
    d.description = "Ingestion, validation, transform, warehouse, API, and BI diagram.";
    d.nodes = {
        {"sources", "external", "Data Sources", 60, 120, 170, 80, "#fb7185", {}},
        {"ingest", "api", "Ingestion API", 320, 120, 180, 80, "#60a5fa", {}},
        {"validate", "decision", "Valid?", 590, 105, 150, 110, "#f59e0b", {}},
        {"deadletter", "database", "Dead Letter", 590, 270, 170, 90, "#a78bfa", {}},
        {"transform", "process", "Transform Jobs", 830, 120, 190, 80, "#38bdf8", {}},
        {"warehouse", "database", "Warehouse", 1100, 120, 180, 90, "#a78bfa", {}},
        {"analytics", "service", "Analytics API", 1370, 120, 180, 80, "#22c55e", {}},
        {"bi", "external", "BI / Reports", 1620, 120, 170, 80, "#fb7185", {}}
    };
    d.edges = {
        {"e_sources_ingest", "sources", "ingest", "batch/webhook", true, "#94a3b8", {}},
        {"e_ingest_validate", "ingest", "validate", "schema", true, "#94a3b8", {}},
        {"e_validate_deadletter", "validate", "deadletter", "reject", true, "#fb7185", {}},
        {"e_validate_transform", "validate", "transform", "accept", true, "#22c55e", {}},
        {"e_transform_warehouse", "transform", "warehouse", "load", true, "#a78bfa", {}},
        {"e_warehouse_analytics", "warehouse", "analytics", "query", true, "#60a5fa", {}},
        {"e_analytics_bi", "analytics", "bi", "serve", true, "#94a3b8", {}}
    };
    return d;
}

} // namespace

const std::vector<DiagramTemplate>& diagramTemplates() {
    static const std::vector<DiagramTemplate> items = {
        {"cloud-architecture", "Cloud Architecture", "API, services, queue, and PostgreSQL reference map.", "Architecture", makeCloudArchitecture()},
        {"incident-response", "Incident Response Flow", "Triage, mitigation, communication, and postmortem flowchart.", "Operations", makeIncidentFlow()},
        {"data-pipeline", "Data Pipeline", "Ingestion, validation, transform, warehouse, API, and BI diagram.", "Data", makeDataPipeline()}
    };
    return items;
}

const DiagramTemplate* findDiagramTemplate(const std::string& key) {
    const auto& items = diagramTemplates();
    auto it = std::find_if(items.begin(), items.end(), [&](const DiagramTemplate& item) {
        return item.key == key;
    });
    return it == items.end() ? nullptr : &*it;
}

} // namespace nuigraph
