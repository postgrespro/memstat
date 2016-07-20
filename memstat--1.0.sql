-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION memstat" to load this file. \quit

CREATE FUNCTION local_memory_stats(
		OUT name text, OUT level int4,
		OUT nblocks int8, OUT freechunks int8, OUT totalspace int8, OUT freespace int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'get_local_memory_stats'
	LANGUAGE C IMMUTABLE;

COMMENT ON FUNCTION local_memory_stats() IS
	'statistic of local memory of backend';

CREATE FUNCTION instance_memory_stats(
		OUT pid int4, OUT name text, OUT level int4,
		OUT nblocks int8, OUT freechunks int8, OUT totalspace int8, OUT freespace int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'get_instance_memory_stats'
	LANGUAGE C IMMUTABLE;

COMMENT ON FUNCTION instance_memory_stats() IS
	'statistic of local memory of all backends';

CREATE OR REPLACE VIEW memory_stats AS
	SELECT
		pid,
		pg_size_pretty(sum(totalspace)) AS totalspace,
		pg_size_pretty(sum(freespace)) AS freespace
	FROM
		instance_memory_stats()
	GROUP BY
		pid;
		

