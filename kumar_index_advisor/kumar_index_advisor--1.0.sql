\echo Use "CREATE EXTENSION kumar_index_advisor" to load this file. \quit

-- Indexes that have never been scanned since statistics were last reset.
-- idx_scan = 0 is a hint for review, not an instruction to DROP INDEX.
CREATE FUNCTION kumar_unused_indexes()
RETURNS TABLE (
    schema_name      text,
    table_name       text,
    index_name       text,
    index_size       text,
    index_scans      bigint,
    index_definition text
)
LANGUAGE sql
STABLE
AS $$
    SELECT
        s.schemaname::text,
        s.relname::text,
        s.indexrelname::text,
        pg_size_pretty(pg_relation_size(s.indexrelid)),
        s.idx_scan,
        pg_get_indexdef(s.indexrelid)
    FROM pg_stat_user_indexes s
    WHERE s.idx_scan = 0
    ORDER BY pg_relation_size(s.indexrelid) DESC;
$$;

-- Pairs of indexes on the same table with identical key columns,
-- operator classes, collations, options, predicate and expressions.
-- Each pair is reported once (i1.oid < i2.oid).
CREATE FUNCTION kumar_duplicate_indexes()
RETURNS TABLE (
    schema_name          text,
    table_name           text,
    index_name           text,
    duplicate_index_name text,
    index_size           text,
    index_definition     text
)
LANGUAGE sql
STABLE
AS $$
    SELECT
        n.nspname::text,
        t.relname::text,
        i1.relname::text,
        i2.relname::text,
        pg_size_pretty(pg_relation_size(i1.oid)),
        pg_get_indexdef(i1.oid)
    FROM pg_index x1
    JOIN pg_index x2
      ON  x2.indrelid = x1.indrelid
      AND x2.indexrelid > x1.indexrelid
      AND x2.indkey::text       = x1.indkey::text
      AND x2.indclass::text     = x1.indclass::text
      AND x2.indcollation::text = x1.indcollation::text
      AND x2.indoption::text    = x1.indoption::text
      AND COALESCE(pg_get_expr(x2.indpred,   x2.indrelid), '') = COALESCE(pg_get_expr(x1.indpred,   x1.indrelid), '')
      AND COALESCE(pg_get_expr(x2.indexprs,  x2.indrelid), '') = COALESCE(pg_get_expr(x1.indexprs,  x1.indrelid), '')
    JOIN pg_class i1 ON i1.oid = x1.indexrelid
    JOIN pg_class i2 ON i2.oid = x2.indexrelid
    JOIN pg_class t  ON t.oid  = x1.indrelid
    JOIN pg_namespace n ON n.oid = t.relnamespace
    WHERE n.nspname NOT IN ('pg_catalog', 'information_schema')
    ORDER BY n.nspname, t.relname, i1.relname;
$$;
