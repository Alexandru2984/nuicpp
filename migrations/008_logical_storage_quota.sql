-- Track user-controlled logical content independently of PostgreSQL page size.
-- The application supplies the canonical current-diagram estimate; version
-- bytes are the compact snapshot plus its note. Triggers keep the global total
-- atomic across inserts, updates, pruning, cascades and direct cleanup.
ALTER TABLE diagrams
    ADD COLUMN IF NOT EXISTS logical_bytes BIGINT NOT NULL DEFAULT 1
    CHECK (logical_bytes > 0);

ALTER TABLE diagram_versions
    ADD COLUMN IF NOT EXISTS logical_bytes BIGINT NOT NULL DEFAULT 1
    CHECK (logical_bytes > 0);

UPDATE diagrams d
SET logical_bytes = GREATEST(
    1,
    octet_length(d.title) + octet_length(d.description)
    + COALESCE((
        SELECT sum(
            octet_length(n.node_key) + octet_length(n.type)
            + octet_length(n.title) + octet_length(n.color)
            + octet_length(n.metadata_json::text)
        )
        FROM nodes n
        WHERE n.diagram_id = d.id
    ), 0)
    + COALESCE((
        SELECT sum(
            octet_length(e.edge_key) + octet_length(e.source_node_key)
            + octet_length(e.target_node_key) + octet_length(e.label)
            + octet_length(e.color) + octet_length(e.metadata_json::text)
        )
        FROM edges e
        WHERE e.diagram_id = d.id
    ), 0)
);

UPDATE diagram_versions
SET logical_bytes = GREATEST(
    1,
    octet_length(snapshot_json::text) + octet_length(note)
);

CREATE TABLE IF NOT EXISTS public.logical_storage_usage (
    singleton BOOLEAN PRIMARY KEY DEFAULT true CHECK (singleton),
    logical_bytes BIGINT NOT NULL CHECK (logical_bytes >= 0)
);

INSERT INTO public.logical_storage_usage (singleton, logical_bytes)
VALUES (
    true,
    (SELECT COALESCE(sum(logical_bytes), 0) FROM diagrams)
    + (SELECT COALESCE(sum(logical_bytes), 0) FROM diagram_versions)
)
ON CONFLICT (singleton) DO UPDATE
SET logical_bytes = EXCLUDED.logical_bytes;

CREATE OR REPLACE FUNCTION public.adjust_logical_storage_usage()
RETURNS trigger
LANGUAGE plpgsql
AS $$
DECLARE
    delta BIGINT;
BEGIN
    IF TG_OP = 'INSERT' THEN
        delta := NEW.logical_bytes;
    ELSIF TG_OP = 'DELETE' THEN
        delta := -OLD.logical_bytes;
    ELSE
        delta := NEW.logical_bytes - OLD.logical_bytes;
    END IF;

    UPDATE public.logical_storage_usage
    SET logical_bytes = logical_bytes + delta
    WHERE singleton = true;

    IF NOT FOUND THEN
        RAISE EXCEPTION 'logical storage usage singleton is missing';
    END IF;

    IF TG_OP = 'DELETE' THEN
        RETURN OLD;
    END IF;
    RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS diagrams_logical_storage_insert_delete ON diagrams;
CREATE TRIGGER diagrams_logical_storage_insert_delete
AFTER INSERT OR DELETE ON diagrams
FOR EACH ROW EXECUTE FUNCTION public.adjust_logical_storage_usage();

DROP TRIGGER IF EXISTS diagrams_logical_storage_update ON diagrams;
CREATE TRIGGER diagrams_logical_storage_update
AFTER UPDATE OF logical_bytes ON diagrams
FOR EACH ROW
WHEN (OLD.logical_bytes IS DISTINCT FROM NEW.logical_bytes)
EXECUTE FUNCTION public.adjust_logical_storage_usage();

DROP TRIGGER IF EXISTS versions_logical_storage_insert_delete ON diagram_versions;
CREATE TRIGGER versions_logical_storage_insert_delete
AFTER INSERT OR DELETE ON diagram_versions
FOR EACH ROW EXECUTE FUNCTION public.adjust_logical_storage_usage();

DROP TRIGGER IF EXISTS versions_logical_storage_update ON diagram_versions;
CREATE TRIGGER versions_logical_storage_update
AFTER UPDATE OF logical_bytes ON diagram_versions
FOR EACH ROW
WHEN (OLD.logical_bytes IS DISTINCT FROM NEW.logical_bytes)
EXECUTE FUNCTION public.adjust_logical_storage_usage();

COMMENT ON TABLE public.logical_storage_usage IS
    'Singleton transactional counter for bounded user-controlled logical bytes';
