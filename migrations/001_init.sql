CREATE TABLE IF NOT EXISTS diagrams (
    id BIGSERIAL PRIMARY KEY,
    title VARCHAR(120) NOT NULL,
    slug VARCHAR(140) NOT NULL UNIQUE,
    description VARCHAR(1000) NOT NULL DEFAULT '',
    owner_token_hash VARCHAR(128),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS nodes (
    id BIGSERIAL PRIMARY KEY,
    diagram_id BIGINT NOT NULL REFERENCES diagrams(id) ON DELETE CASCADE,
    node_key VARCHAR(64) NOT NULL,
    type VARCHAR(32) NOT NULL,
    title VARCHAR(160) NOT NULL,
    x DOUBLE PRECISION NOT NULL,
    y DOUBLE PRECISION NOT NULL,
    width DOUBLE PRECISION NOT NULL,
    height DOUBLE PRECISION NOT NULL,
    color VARCHAR(7) NOT NULL,
    metadata_json JSONB NOT NULL DEFAULT '{}'::jsonb,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (diagram_id, node_key)
);

CREATE TABLE IF NOT EXISTS edges (
    id BIGSERIAL PRIMARY KEY,
    diagram_id BIGINT NOT NULL REFERENCES diagrams(id) ON DELETE CASCADE,
    edge_key VARCHAR(64) NOT NULL,
    source_node_key VARCHAR(64) NOT NULL,
    target_node_key VARCHAR(64) NOT NULL,
    label VARCHAR(160) NOT NULL DEFAULT '',
    directed BOOLEAN NOT NULL DEFAULT true,
    color VARCHAR(7) NOT NULL,
    metadata_json JSONB NOT NULL DEFAULT '{}'::jsonb,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (diagram_id, edge_key)
);

CREATE INDEX IF NOT EXISTS idx_nodes_diagram ON nodes(diagram_id);
CREATE INDEX IF NOT EXISTS idx_edges_diagram ON edges(diagram_id);

CREATE TABLE IF NOT EXISTS diagram_versions (
    id BIGSERIAL PRIMARY KEY,
    diagram_id BIGINT NOT NULL REFERENCES diagrams(id) ON DELETE CASCADE,
    version_number INTEGER NOT NULL,
    snapshot_json JSONB NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    note VARCHAR(240) NOT NULL DEFAULT '',
    UNIQUE (diagram_id, version_number)
);

CREATE INDEX IF NOT EXISTS idx_versions_diagram ON diagram_versions(diagram_id, version_number DESC);
