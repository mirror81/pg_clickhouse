#ifndef CLICKHOUSE_ENGINE_H
#define CLICKHOUSE_ENGINE_H

#include "kv_list.h"

/*
 * ch_connection_details defines the details for connecting to ClickHouse.
 */
typedef struct
{
	char	   *host;
	int			port;
	char	   *username;
	char	   *password;
	char	   *dbname;
}			ch_connection_details;

/*
 * ch_query an SQL query to execute on ClickHouse.
 */
typedef struct
{
	const char *sql;
	const int	num_params;
	const char **param_values;
	const		kv_list *settings;
}			ch_query;

#define new_query(sql, num, vals) {sql, num, vals, chfdw_get_session_settings()}

#endif							/* CLICKHOUSE_ENGINE_H */
