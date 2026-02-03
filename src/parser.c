#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <curl/curl.h>

#include <http.h>
#include <internal.h>

void
ch_http_read_state_init(ch_http_read_state * state, char *data, size_t datalen)
{
	state->data = datalen > 0 ? data : NULL;
	state->maxpos = datalen - 1;
	state->val = makeStringInfo();
	state->done = false;
}

void
ch_http_read_state_free(ch_http_read_state * state)
{
	/* destroyStringInfo not added till Postgres 17 */
	pfree(state->val->data);
	pfree(state->val);
}

int
ch_http_read_next(ch_http_read_state * state)
{
	size_t		pos = state->curpos;
	char	   *data = state->data;

	resetStringInfo(state->val);
	if (state->done)
		return CH_EOF;

	while (pos < state->maxpos && data[pos] != '\t' && data[pos] != '\n')
	{
		if (data[pos] == '\\')
		{
			pos++;
			/* unescape some sequences */
			switch (data[pos])
			{
				case 'n':
					appendStringInfoChar(state->val, '\n');
					break;
				case 't':
					appendStringInfoChar(state->val, '\t');
					break;
				case '0':
					appendStringInfoChar(state->val, '\0');
					break;
				case 'r':
					appendStringInfoChar(state->val, '\r');
					break;
				case 'b':
					appendStringInfoChar(state->val, '\b');
					break;
				case 'f':
					appendStringInfoChar(state->val, '\f');
					break;
				case 'N':
					/* NULL (format_tsv_null_representation) */
					appendStringInfoString(state->val, "\\N");
					break;
				default:
					appendStringInfoChar(state->val, data[pos]);
			}
			pos++;
		}
		else
			appendStringInfoChar(state->val, data[pos++]);
	}

	state->curpos = pos + 1;

	if (data[pos] == '\t')
		return CH_CONT;

	Assert(data[pos] == '\n');
	int			res = pos < state->maxpos ? CH_EOL : CH_EOF;

	state->done = (res == CH_EOF);

	return res;
}
