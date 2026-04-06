#ifndef CLICKHOUSE_HTTP_STREAMING_H
#define CLICKHOUSE_HTTP_STREAMING_H

#include "engine.h"

typedef struct ch_http_connection_t ch_http_connection_t;

 /*
  * Opaque handle to a streaming HTTP query. The real type is the HttpStream
  * struct, defined in http_streaming.c.
  */
typedef struct HttpStream HttpStream;

 /* lifecycle */
HttpStream *ch_http_stream_begin(ch_http_connection_t * conn,
								 const ch_query * query,
								 int32 fetch_size);
int			ch_http_stream_pump(HttpStream * stream);
void		ch_http_stream_end(HttpStream * stream);

 /* accessors — let pglink.c read stream state without seeing the struct */
char	   *ch_http_stream_buffer(HttpStream * stream);
size_t		ch_http_stream_available(HttpStream * stream);
void		ch_http_stream_advance(HttpStream * stream, size_t n);
bool		ch_http_stream_transfer_done(HttpStream * stream);
long		ch_http_stream_status(HttpStream * stream);
const char *ch_http_stream_query_id(HttpStream * stream);
const char *ch_http_stream_error(HttpStream * stream);
double		ch_http_stream_request_time(HttpStream * stream);
double		ch_http_stream_total_time(HttpStream * stream);

#endif							/* CLICKHOUSE_HTTP_STREAMING_H */
