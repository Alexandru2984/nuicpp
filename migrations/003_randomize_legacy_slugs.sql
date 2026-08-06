-- Share slugs are bearer capabilities: /d/<slug> hands the full diagram to
-- anyone who presents it, with no further check. Slugs created before the
-- switch to random suffixes ended in a unix timestamp, so a visitor who knew
-- roughly when a diagram was made could enumerate the remaining digits and
-- reach diagrams that were never shared with them.
--
-- Re-suffix only those rows. Slugs already carrying a 16-hex-character random
-- suffix are left alone, so this is safe to re-run and does not invalidate
-- links handed out since that change. Any /d/ link to an affected diagram
-- stops working, which is the point.
--
-- gen_random_uuid() is built in from PostgreSQL 13 and draws on the platform
-- CSPRNG, so it needs no extension; random() would be predictable and is
-- exactly what this migration exists to get away from.

UPDATE diagrams
SET slug = regexp_replace(slug, '-[0-9]{9,11}$', '')
           || '-'
           || left(replace(gen_random_uuid()::text, '-', ''), 16)
WHERE slug ~ '-[0-9]{9,11}$';
