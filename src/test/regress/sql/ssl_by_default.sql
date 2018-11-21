-- Citus uses ssl by default now. It does so by turning on ssl and if needed generate self
-- signed certificates.

-- To test this we will verify that SSL is set to ON for all machines, and we will make
-- sure connections to workers use SSL by having it required in citus.conn_nodeinfo and
-- lastly we will inspect the ssl state for connections to the workers

-- ssl can only be enabled by default on installations of postgres 10 and above that has
-- ssl linked during compile time. When ssl is not linked during compile time it defaults
-- the ssl_ciphers setting to 'none'.
SHOW server_version \gset
SHOW ssl_ciphers \gset
WITH features AS (
    SELECT
        substring(:'server_version', '\d+')::int >= 10 AS version_ten_or_above,
        :'ssl_ciphers' != 'none' AS hasssl
)
SELECT (
    true
    AND version_ten_or_above
    AND hasssl
) AS ssl_by_default_supported FROM features;

-- we expect all nodes to have ssl on
SHOW ssl;
SELECT run_command_on_workers($$
    SHOW ssl;
$$);

-- we expect all nodes to have node_conninfo set to 'sslmode=require'
SHOW citus.node_conninfo;
SELECT run_command_on_workers($$
    SHOW citus.node_conninfo;
$$);

-- we expect all connections to the workers have ssl on
SELECT run_command_on_workers($$
    SELECT ssl FROM pg_stat_ssl WHERE pid = pg_backend_pid();
$$);
