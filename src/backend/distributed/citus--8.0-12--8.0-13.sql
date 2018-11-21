/* citus--8.0-12--8.0-13 */
CREATE FUNCTION citus_reset_default_for_node_conninfo()
    RETURNS void
    LANGUAGE C STRICT
    AS 'MODULE_PATHNAME', $$citus_reset_default_for_node_conninfo$$;

DO LANGUAGE plpgsql
$$
BEGIN
    -- citus requires ssl form this version onwards. For older installations that didn't
    -- change the citus.node_conninfo setting this will cause the new default value to be
    -- used.
    -- This default value is incompatible with postgres' default setting for ssl. When we
    -- detect that citus.node_conninfo is exactly the same value as the default with ssl
    -- not turned on we assume it is an upgrade of an older version.
    -- To not impose the overhead of ssl on customers upgrading we change
    -- citus.node_conninfo back to the old default.
    -- During a clean installation of citus on postgres we enable ssl and when required
    -- generate self signed certificates. When we later hit this point ssl is set to 'on'
    -- so it will not perform the reset.
    IF
        TRUE -- hack to easily move around the checks below

		AND current_setting('ssl_ciphers') != 'none' -- test if ssl is compiled into postgres
		AND NOT current_setting('ssl')::boolean -- test if ssl is off
		AND current_setting('citus.node_conninfo') = 'sslmode=require' -- test citus.node_conninfo is set to the default value of sslmode=prefer
	THEN
	    PERFORM citus_reset_default_for_node_conninfo();
	END IF;
END;
$$;

DROP FUNCTION citus_reset_default_for_node_conninfo();