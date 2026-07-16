/*
 * insert.c
 *
 * Handle binary INSERT lifecycle
 *
 *  -begin_insert sends query and uses server's empty Data block as schema
 * - append functions buffer column values in format chosen from schema
 * - flush_block assembles buffered columns and sends one Data packet
 * - finalize_insert sends closing empty Data packet and reads final server response
 * - release_insert can run during memory context reset and never writes to connection.
 */

#include "postgres.h"

#include <string.h>

#include "common/hashfn.h"
#include "port/pg_bswap.h"
#include "utils/memutils.h"
#include "utils/palloc.h"

#include "binary_internal.h"

/* dynamic buffer for raw bytes; palloc-backed, freed by context delete */
typedef struct dynbuf {
    uint8_t* data;
    size_t len;
    size_t cap;
} dynbuf;

static void
dynbuf_reserve(dynbuf* b, size_t need) {
    if (need <= b->cap) {
        return;
    }
    size_t ncap = b->cap ? b->cap : 64;

    while (ncap < need) {
        ncap *= 2;
    }
    b->data =
        b->data
            ? repalloc_huge(b->data, ncap)
            : MemoryContextAllocExtended(CurrentMemoryContext, ncap, MCXT_ALLOC_HUGE);
    b->cap = ncap;
}

static void
dynbuf_append(dynbuf* b, const void* src, size_t n) {
    dynbuf_reserve(b, b->len + n);
    if (src && n) {
        memcpy(b->data + b->len, src, n);
    }
    b->len += n;
}

static void
dynbuf_append_zero(dynbuf* b, size_t n) {
    dynbuf_reserve(b, b->len + n);
    memset(b->data + b->len, 0, n);
    b->len += n;
}

static void
dynbuf_reset(dynbuf* b) {
    b->len = 0;
}

/* dynamic array of u64 */
typedef struct u64buf {
    uint64_t* data;
    size_t len;
    size_t cap;
} u64buf;

static void
u64buf_push(u64buf* b, uint64_t v) {
    if (b->len + 1 > b->cap) {
        size_t ncap  = b->cap ? b->cap * 2 : 16;
        size_t bytes = ncap * sizeof(uint64_t);

        b->data = b->data ? repalloc_huge(b->data, bytes)
                          : MemoryContextAllocExtended(
                                CurrentMemoryContext, bytes, MCXT_ALLOC_HUGE
                            );
        b->cap  = ncap;
    }
    b->data[b->len++] = v;
}

static void
u64buf_reset(u64buf* b) {
    b->len = 0;
}

/*
 * Buffer-node tree mirroring the column's chc_type, one node per CH
 * structural level. Typed append_* functions write leaves, array begin/end
 * records offsets on Array nodes, flush assembles chc_build_* bottom-up
 * from the same shape.
 */
typedef enum {
    ICN_FIXED,
    ICN_STRING,
    ICN_NULLABLE,
    ICN_ARRAY,
    ICN_LC,
} icn_kind;

typedef struct icn icn;

struct icn {
    icn_kind kind;
    const chc_type* type; /* this level's type; source of elem size, enum
                           * table, decimal scale, dt64 precision */
    union {
        struct {
            dynbuf data; /* row-aligned values */
            size_t elem_size;
            uint32_t dt64_precision;
        } fixed;

        struct {
            dynbuf data; /* byte-flat rows */
            u64buf offs; /* cumulative ends per row */
            bool is_json;
        } str;

        struct {
            dynbuf null_map; /* one byte per row */
            icn* inner;
        } nullable;

        struct {
            u64buf offs; /* cumulative ends per committed row */
            icn* values;
        } array;

        struct {
            dynbuf data; /* raw rows; dict dedup happens at flush */
            u64buf offs;
            dynbuf null_map; /* only when inner_nullable */
            bool inner_nullable;
        } lc;
    };
};

typedef struct ic_col {
    const chc_type* t; /* full column type (incl. Nullable wrapper) */
    icn* root;

    /*
     * Cached column info exposed to callers. info.type borrows from the
     * initial_block's chc_type tree, unwrapping Nullable + outer LC.
     */
    ch_binary_column_info info;
} ic_col;

struct ch_binary_insert_handle {
    MemoryContext cxt;
    chc_client* client;
    struct ch_binary_state* state; /* parent connection; used to flag broken
                                    * state on error */
    chc_block* initial_block;      /* schema source (server's empty Data) */
    size_t ncols;
    ic_col* cols;
    icn** cursor; /* stack of open Array nodes; top's values child
                   * receives appends. Empty at top level */
    size_t cursor_len;
    size_t cursor_cap;
    size_t cursor_col; /* column that opened cursor[0] */
    bool started;
    bool finalized; /* finalize_insert has run (success or raised) */
};

/* Build one buffer node per structural level of `t` */
static icn*
build_node(const chc_type* t) {
    icn* n = palloc0(sizeof(icn));

    n->type = t;
    switch (chc_type_kind(t)) {
    case CHC_NULLABLE:
        n->kind           = ICN_NULLABLE;
        n->nullable.inner = build_node(chc_type_child(t, 0));
        return n;
    case CHC_ARRAY:
        n->kind         = ICN_ARRAY;
        n->array.values = build_node(chc_type_child(t, 0));
        return n;
    case CHC_LOW_CARDINALITY: {
        /*
         * Nullable lives inside LowCardinality, not as a wrapper above it:
         * resolve_leaf tracks the per-row null bits build_lc_dict reads to
         * map nulls onto dict slot 0.
         */
        const chc_type* inner = chc_type_child(t, 0);
        bool inner_nullable   = chc_type_kind(inner) == CHC_NULLABLE;
        const chc_type* base  = inner_nullable ? chc_type_child(inner, 0) : inner;

        if (chc_type_kind(base) != CHC_STRING) {
            ereport(
                ERROR,
                errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
                errmsg(
                    "pg_clickhouse: unsupported LowCardinality variant: %s",
                    chc_type_name(base, NULL)
                )
            );
        }
        n->kind              = ICN_LC;
        n->lc.inner_nullable = inner_nullable;
        return n;
    }
    case CHC_STRING:
        n->kind = ICN_STRING;
        return n;
    case CHC_JSON:
        n->kind        = ICN_STRING;
        n->str.is_json = true;
        return n;
    case CHC_DATETIME64: {
        int scale = chc_type_datetime64_scale(t);

        if (scale < 0 || (size_t)scale >= lengthof(pow10i)) {
            ereport(
                ERROR,
                errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
                errmsg("pg_clickhouse: DateTime64 scale %d out of range", scale)
            );
        }
        n->kind                 = ICN_FIXED;
        n->fixed.elem_size      = chc_type_elem_size(t);
        n->fixed.dt64_precision = (uint32_t)scale;
        return n;
    }
    default: {
        size_t es = chc_type_elem_size(t);

        if (es == 0) {
            ereport(
                ERROR,
                errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
                errmsg(
                    "pg_clickhouse: could not prepare insert - unsupported column "
                    "type: %s",
                    chc_type_name(t, NULL)
                )
            );
        }
        n->kind            = ICN_FIXED;
        n->fixed.elem_size = es;
        return n;
    }
    }
}

static void
recv_initial_block(struct ch_binary_state* s, ch_binary_insert_handle* h) {
    for (;;) {
        chc_packet pkt = {};
        chc_err err    = {};
        int rc         = chc_client_recv_packet(s->client, &pkt, &err);

        if (rc != CHC_OK) {
            s->broken = true;
            raise_chc(&err, ERRCODE_FDW_ERROR, "could not prepare insert - ");
        }
        if (pkt.kind == CHC_PKT_EXCEPTION) {
            const char* msg = "server exception";

            if (pkt.exception && pkt.exception->display_text) {
                msg = pkt.exception->display_text;
            } else if (pkt.exception && pkt.exception->name) {
                msg = pkt.exception->name;
            }
            char* msg_copy = pstrdup(msg);

            s->broken = true;
            chc_packet_clear(s->client, &pkt);
            ereport(
                ERROR,
                errcode(ERRCODE_FDW_ERROR),
                errmsg("pg_clickhouse: could not prepare insert - %s", msg_copy)
            );
        }
        if (pkt.kind == CHC_PKT_DATA && pkt.block &&
            chc_block_n_columns(pkt.block) > 0) {
            h->initial_block = pkt.block;
            pkt.block        = NULL;
            chc_packet_clear(s->client, &pkt);
            return;
        }
        chc_packet_clear(s->client, &pkt);
    }
}

/*
 * After build_node ereports the server is mid-INSERT awaiting our
 * Data; send empty Data + drain so the connection stays usable.
 */
static void
drain_aborted_insert(struct ch_binary_state* s) {
    chc_err ce = {};

    (void)chc_client_send_data(s->client, NULL, &ce);
    for (;;) {
        chc_packet drain = {};

        ce      = (chc_err){ 0 };
        int drc = chc_client_recv_packet(s->client, &drain, &ce);
        bool eos =
            (drc == CHC_OK &&
             (drain.kind == CHC_PKT_END_OF_STREAM || drain.kind == CHC_PKT_EXCEPTION));

        chc_packet_clear(s->client, &drain);
        if (drc != CHC_OK || eos) {
            break;
        }
    }
}

ch_binary_insert_handle*
ch_binary_begin_insert(
    ch_binary_connection_t* conn,
    const ch_query* query,
    ch_binary_column_info** out_cols,
    size_t* out_n
) {
    struct ch_binary_state* s = conn_state(conn);

    /*
     * Parent h's cxt to the connection's cxt, not CurrentMemoryContext. The
     * caller registers a reset callback on a sibling context that drains the
     * insert via end_insert(h); if h lived under that sibling, MemoryContext
     * tree teardown would free h before the callback fired.
     */
    MemoryContext cxt = AllocSetContextCreate(
        s->cxt, "pg_clickhouse binary insert", ALLOCSET_DEFAULT_SIZES
    );
    MemoryContext old = MemoryContextSwitchTo(cxt);
    ch_binary_insert_handle* h;
    volatile bool need_drain = false;

    PG_TRY();
    {
        h         = palloc0(sizeof(*h));
        h->cxt    = cxt;
        h->client = s->client;
        h->state  = s;

        /* Append " VALUES" so server enters insert mode. */
        size_t sql_len = strlen(query->sql);
        char* sql      = palloc(sql_len + 8);

        memcpy(sql, query->sql, sql_len);
        memcpy(sql + sql_len, " VALUES", 7);
        sql[sql_len + 7] = '\0';

        /*
         * On servers that support it (24.10+), tell server to serialize any
         * JSON columns using STRING wire format. INSERT path doesn't need
         * this, server reads the per-column version prefix the builder
         * writes, but we set it on the same packet for symmetry with the
         * SELECT path and so any RETURNING-style projection on top still
         * decodes.
         */
        chc_query_setting json_setting = {
            .name      = "output_format_native_write_json_as_string",
            .value     = "1",
            .important = true,
        };
        chc_query_opts insert_opts     = {};
        const chc_query_opts* opts_ptr = NULL;

        if (server_supports_json_as_string(s->client)) {
            insert_opts.settings   = &json_setting;
            insert_opts.n_settings = 1;
            opts_ptr               = &insert_opts;
        }

        chc_err err = {};
        int rc = chc_client_send_query_ex(s->client, sql, sql_len + 7, opts_ptr, &err);

        if (rc != CHC_OK) {
            s->broken = true;
            raise_chc(&err, ERRCODE_FDW_ERROR, "could not prepare insert - ");
        }

        recv_initial_block(s, h);

        /*
         * Server is now waiting our Data; failures past this point need an
         * empty-Data + drain so the connection stays usable.
         */
        need_drain = true;
        h->started = true;

        size_t nc = chc_block_n_columns(h->initial_block);

        h->ncols = nc;
        h->cols  = palloc0(nc * sizeof(ic_col));
        for (size_t i = 0; i < nc; i++) {
            ic_col* c          = &h->cols[i];
            const chc_type* ct = chc_block_column_type(h->initial_block, i);
            size_t nlen;
            const char* nm;

            c->t         = ct;
            c->root      = build_node(ct);
            nm           = chc_block_column_name(h->initial_block, i, &nlen);
            c->info.name = pnstrdup(nm ? nm : "", nlen);

            /*
             * Unwrap Nullable, then LowCardinality and perhaps its inner
             * Nullable to expose innermost type. Nullable inside LC counts
             * as column nullability.
             */
            const chc_type* vt = ct;
            bool is_nullable   = false;

            if (chc_type_kind(vt) == CHC_NULLABLE) {
                is_nullable = true;
                vt          = chc_type_child(vt, 0);
            }
            if (chc_type_kind(vt) == CHC_LOW_CARDINALITY) {
                vt          = chc_type_child(vt, 0);
                is_nullable = chc_type_kind(vt) == CHC_NULLABLE;
                if (is_nullable) {
                    vt = chc_type_child(vt, 0);
                }
            }
            c->info.type        = vt;
            c->info.is_nullable = is_nullable;
        }

        *out_cols = NULL;
        if (nc) {
            ch_binary_column_info* arr = palloc(nc * sizeof(*arr));

            for (size_t i = 0; i < nc; i++) {
                arr[i] = h->cols[i].info;
            }
            *out_cols = arr;
        }
        *out_n = nc;
    }
    PG_CATCH();
    {
        if (need_drain) {
            drain_aborted_insert(s);
        }
        MemoryContextSwitchTo(old);
        MemoryContextDelete(cxt);
        PG_RE_THROW();
    }
    PG_END_TRY();

    MemoryContextSwitchTo(old);
    return h;
}

/* Node receiving values: innermost open array's values child, else column root */
static inline icn*
cursor_node(const ch_binary_insert_handle* h, size_t col) {
    return h->cursor_len ? h->cursor[h->cursor_len - 1]->array.values
                         : h->cols[col].root;
}

/*
 * Descend from the append target to its leaf FIXED/STRING/LC node,
 * recording a null bit on each Nullable level crossed (or on
 * LowCardinality(Nullable(...))). ereports on NULL-into-NOT-NULL.
 */
static icn*
resolve_leaf(ch_binary_insert_handle* h, size_t col, bool isnull) {
    icn* node     = cursor_node(h, col);
    uint8_t b     = isnull ? 1 : 0;
    bool nullable = false;

    while (node->kind == ICN_NULLABLE) {
        dynbuf_append(&node->nullable.null_map, &b, 1);
        nullable = true;
        node     = node->nullable.inner;
    }
    if (node->kind == ICN_LC && node->lc.inner_nullable) {
        dynbuf_append(&node->lc.null_map, &b, 1);
        nullable = true;
    }
    if (isnull && !nullable) {
        const chc_type* t = h->cols[h->cursor_len ? h->cursor_col : col].t;
        size_t tnlen;
        const char* tname = chc_type_name(t, &tnlen);

        ereport(
            ERROR,
            errcode(ERRCODE_NOT_NULL_VIOLATION),
            errmsg(
                "pg_clickhouse: cannot append NULL to NOT NULL %.*s column",
                (int)tnlen,
                tname ? tname : "?"
            )
        );
    }
    if (node->kind == ICN_ARRAY) {
        ereport(
            ERROR,
            errcode(ERRCODE_DATATYPE_MISMATCH),
            errmsg("pg_clickhouse: scalar value into Array column")
        );
    }
    return node;
}

/* Leaf buffer for fixed-width appends; guards union access on misdispatch */
static dynbuf*
fixed_data(icn* node) {
    if (node->kind != ICN_FIXED) {
        ereport(
            ERROR,
            errcode(ERRCODE_DATATYPE_MISMATCH),
            errmsg("pg_clickhouse: fixed-width value into non-fixed-width column")
        );
    }
    return &node->fixed.data;
}

/* STRING & LC rows: append bytes, record cumulative end */
static void
append_row_offs(dynbuf* data, u64buf* offs, const void* p, size_t n) {
    if (n) {
        dynbuf_append(data, p, n);
    }
    u64buf_push(offs, data->len);
}

/*
 * Convert decimal text "[-]digits[.frac]" into a ClickHouse Decimal wire value:
 * two's-complement signed integer in `width` LE bytes (4/8/16/32 for
 * Decimal32/64/128/256), with `scale` fractional digits folded into the value.
 */
static void
decimal_text_to_bytes(const char* s, uint32_t scale, size_t width, uint8_t* out) {
    const char* input = s;
    bool neg          = false;

    if (!s) {
        ereport(
            ERROR,
            errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
            errmsg("pg_clickhouse: decimal parse failure")
        );
    }
    if (*s == '-') {
        neg = true;
        s++;
    } else if (*s == '+') {
        s++;
    }

    /* find offsets, going to iterate digits, skipping non-digits */
    const char* dot  = strchr(s, '.');
    size_t ilen      = dot ? (size_t)(dot - s) : strlen(s);
    const char* frac = dot ? dot + 1 : "";
    size_t flen      = strlen(frac);
    size_t ndig      = ilen + scale;

    uint32_t mag[8] = {};
    size_t nwords   = width / 4;

    /* accumulate digits (padded/truncated to scale) into mag */
    for (size_t i = 0; i < ndig; i++) {
        char c = i < ilen ? s[i] : i - ilen < flen ? frac[i - ilen] : '0';

        /* reject NaN / Infinity from numeric_out */
        if (c < '0' || c > '9') {
            ereport(
                ERROR,
                errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                errmsg(
                    "pg_clickhouse: cannot encode \"%s\" as ClickHouse Decimal", input
                )
            );
        }
        uint64_t carry = (uint64_t)(c - '0');

        for (size_t b = 0; b < nwords; b++) {
            uint64_t v = (uint64_t)mag[b] * 10 + carry;

            mag[b] = (uint32_t)v;
            carry  = v >> 32;
        }
    }
    /* two's-complement negation */
    if (neg) {
        for (size_t b = 0; b < nwords; b++) {
            mag[b] = ~mag[b];
        }
        uint64_t carry = 1;

        for (size_t b = 0; b < nwords && carry; b++) {
            uint64_t v = (uint64_t)mag[b] + carry;

            mag[b] = (uint32_t)v;
            carry  = v >> 32;
        }
    }

    memcpy(out, mag, width);
}

static void
append_int_kind(icn* node, int64_t val) {
    switch (chc_type_kind(node->type)) {
    case CHC_INT8:
    case CHC_UINT8:
    case CHC_BOOL: {
        int8_t v = (int8_t)val;

        dynbuf_append(fixed_data(node), &v, 1);
        return;
    }
    case CHC_INT16:
    case CHC_UINT16: {
        int16_t v = (int16_t)val;

        dynbuf_append(fixed_data(node), &v, 2);
        return;
    }
    case CHC_INT32:
    case CHC_UINT32: {
        int32_t v = (int32_t)val;

        dynbuf_append(fixed_data(node), &v, 4);
        return;
    }
    case CHC_INT64:
    case CHC_UINT64: {
        dynbuf_append(fixed_data(node), &val, 8);
        return;
    }
    default:
        ereport(
            ERROR,
            errcode(ERRCODE_DATATYPE_MISMATCH),
            errmsg("pg_clickhouse: int value into non-integer column")
        );
    }
}

void
ch_binary_append_int(ch_binary_insert_handle* h, size_t col, int64_t val, bool isnull) {
    MemoryContext old = MemoryContextSwitchTo(h->cxt);
    icn* node         = resolve_leaf(h, col, isnull);

    if (isnull) {
        dynbuf* d = fixed_data(node);

        dynbuf_append_zero(d, node->fixed.elem_size);
    } else {
        append_int_kind(node, val);
    }
    MemoryContextSwitchTo(old);
}

void
ch_binary_append_uint(
    ch_binary_insert_handle* h,
    size_t col,
    uint64_t val,
    bool isnull
) {
    ch_binary_append_int(h, col, (int64_t)val, isnull);
}

void
ch_binary_append_bool(ch_binary_insert_handle* h, size_t col, bool val, bool isnull) {
    ch_binary_append_int(h, col, val, isnull);
}

void
ch_binary_append_double(
    ch_binary_insert_handle* h,
    size_t col,
    double val,
    bool isnull
) {
    MemoryContext old = MemoryContextSwitchTo(h->cxt);
    icn* node         = resolve_leaf(h, col, isnull);

    if (isnull) {
        dynbuf_append_zero(fixed_data(node), 8);
    } else {
        dynbuf_append(fixed_data(node), &val, 8);
    }
    MemoryContextSwitchTo(old);
}

void
ch_binary_append_float(ch_binary_insert_handle* h, size_t col, float val, bool isnull) {
    MemoryContext old = MemoryContextSwitchTo(h->cxt);
    icn* node         = resolve_leaf(h, col, isnull);

    if (isnull) {
        dynbuf_append_zero(fixed_data(node), 4);
    } else {
        dynbuf_append(fixed_data(node), &val, 4);
    }
    MemoryContextSwitchTo(old);
}

/* FixedString width-pads, Enum maps name to value via node's type table */
static void
append_bytes_fixed(icn* node, const void* p, size_t n, bool isnull) {
    chc_kind k   = chc_type_kind(node->type);
    dynbuf* data = &node->fixed.data;

    if (k == CHC_FIXED_STRING) {
        size_t w = node->fixed.elem_size;

        if (isnull) {
            dynbuf_append_zero(data, w);
            return;
        }
        size_t take = n < w ? n : w;

        if (take) {
            dynbuf_append(data, p, take);
        }
        if (take < w) {
            dynbuf_append_zero(data, w - take);
        }
        return;
    }
    if (k == CHC_ENUM8 || k == CHC_ENUM16) {
        if (isnull) {
            dynbuf_append_zero(data, node->fixed.elem_size);
            return;
        }
        size_t nenum = chc_type_enum_count(node->type);
        int64_t val  = 0;
        bool found   = false;

        for (size_t i = 0; i < nenum; i++) {
            const char* en;
            size_t el;
            int64_t ev;

            chc_type_enum_at(node->type, i, &en, &el, &ev);
            if (el == n && memcmp(en, p, n) == 0) {
                val   = ev;
                found = true;
                break;
            }
        }
        if (!found) {
            ereport(
                ERROR,
                errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                errmsg(
                    "pg_clickhouse: enum value '%.*s' not found", (int)n, (const char*)p
                )
            );
        }
        if (k == CHC_ENUM8) {
            int8_t v = (int8_t)val;

            dynbuf_append(data, &v, 1);
        } else {
            int16_t v = (int16_t)val;

            dynbuf_append(data, &v, 2);
        }
        return;
    }
    ereport(
        ERROR,
        errcode(ERRCODE_DATATYPE_MISMATCH),
        errmsg("pg_clickhouse: bytes into non-text column")
    );
}

void
ch_binary_append_bytes(
    ch_binary_insert_handle* h,
    size_t col,
    const void* p,
    size_t n,
    bool isnull
) {
    MemoryContext old = MemoryContextSwitchTo(h->cxt);
    icn* node         = resolve_leaf(h, col, isnull);

    switch (node->kind) {
    case ICN_LC:
        /* Buffer values and offsets, build LowCardinality dictionary during flush */
        append_row_offs(
            &node->lc.data, &node->lc.offs, isnull ? NULL : p, isnull ? 0 : n
        );
        break;
    case ICN_STRING:
        if (isnull && node->str.is_json) {
            /* CH still parses Nullable's values, choking on invalid JSON */
            append_row_offs(&node->str.data, &node->str.offs, "{}", 2);
        } else {
            append_row_offs(
                &node->str.data, &node->str.offs, isnull ? NULL : p, isnull ? 0 : n
            );
        }
        break;
    case ICN_FIXED:
        append_bytes_fixed(node, p, n, isnull);
        break;
    default:
        ereport(
            ERROR,
            errcode(ERRCODE_DATATYPE_MISMATCH),
            errmsg("pg_clickhouse: bytes into non-text column")
        );
    }
    MemoryContextSwitchTo(old);
}

void
ch_binary_append_decimal(
    ch_binary_insert_handle* h,
    size_t col,
    const char* digits,
    bool isnull
) {
    MemoryContext old = MemoryContextSwitchTo(h->cxt);
    icn* node         = resolve_leaf(h, col, isnull);
    size_t w;

    switch (chc_type_kind(node->type)) {
    case CHC_DECIMAL32:
        w = 4;
        break;
    case CHC_DECIMAL64:
        w = 8;
        break;
    case CHC_DECIMAL128:
        w = 16;
        break;
    case CHC_DECIMAL256:
        w = 32;
        break;
    default:
        ereport(
            ERROR,
            errcode(ERRCODE_DATATYPE_MISMATCH),
            errmsg("pg_clickhouse: decimal into non-decimal column")
        );
    }
    uint8_t raw[32] = {};

    if (!isnull && digits) {
        uint32_t scale = (uint32_t)chc_type_decimal_scale(node->type);

        decimal_text_to_bytes(digits, scale, w, raw);
    }
    dynbuf_append(&node->fixed.data, raw, w);
    MemoryContextSwitchTo(old);
}

void
ch_binary_append_uuid(
    ch_binary_insert_handle* h,
    size_t col,
    const uint8_t bytes[16],
    bool isnull
) {
    MemoryContext old = MemoryContextSwitchTo(h->cxt);
    icn* node         = resolve_leaf(h, col, isnull);
    uint8_t wire[16]  = {};

    if (!isnull) {
        uint64_t a, b;

        memcpy(&a, bytes, 8);
        memcpy(&b, bytes + 8, 8);
        a = pg_ntoh64(a);
        b = pg_ntoh64(b);
        memcpy(wire, &a, 8);
        memcpy(wire + 8, &b, 8);
    }
    dynbuf_append(fixed_data(node), wire, 16);
    MemoryContextSwitchTo(old);
}

/*
 * addr_be is BE bytes (PG inet ip_addr layout). For IPv4 CH wire is a
 * host-order uint32; pg_ntoh32 turns BE bytes into the right host value.
 * For IPv6 CH wire matches PG byte order.
 */
void
ch_binary_append_inet(
    ch_binary_insert_handle* h,
    size_t col,
    const uint8_t* addr_be,
    size_t addrlen,
    bool isnull
) {
    MemoryContext old = MemoryContextSwitchTo(h->cxt);
    icn* node         = resolve_leaf(h, col, isnull);
    chc_kind k        = chc_type_kind(node->type);

    if (k == CHC_IPV4 && addrlen == 4) {
        uint32_t addr = 0;

        if (!isnull && addr_be) {
            uint32_t be;

            memcpy(&be, addr_be, 4);
            addr = pg_ntoh32(be);
        }
        dynbuf_append(&node->fixed.data, &addr, 4);
        MemoryContextSwitchTo(old);
        return;
    }
    if (k == CHC_IPV6 && addrlen == 16) {
        uint8_t raw[16] = {};

        if (!isnull && addr_be) {
            memcpy(raw, addr_be, 16);
        }
        dynbuf_append(&node->fixed.data, raw, 16);
        MemoryContextSwitchTo(old);
        return;
    }
    ereport(
        ERROR,
        errcode(ERRCODE_DATATYPE_MISMATCH),
        errmsg("pg_clickhouse: cannot insert inet into non-inet column")
    );
}

void
ch_binary_append_date_seconds(
    ch_binary_insert_handle* h,
    size_t col,
    int64_t seconds,
    bool isnull
) {
    MemoryContext old = MemoryContextSwitchTo(h->cxt);
    icn* node         = resolve_leaf(h, col, isnull);
    chc_kind k        = chc_type_kind(node->type);

    if (k == CHC_DATE) {
        uint16_t days = isnull ? 0 : (uint16_t)(seconds / 86400);

        dynbuf_append(&node->fixed.data, &days, 2);
    } else if (k == CHC_DATE32) {
        int32_t days = isnull ? 0 : (int32_t)(seconds / 86400);

        dynbuf_append(&node->fixed.data, &days, 4);
    } else {
        ereport(
            ERROR,
            errcode(ERRCODE_DATATYPE_MISMATCH),
            errmsg("pg_clickhouse: date into non-date column")
        );
    }
    MemoryContextSwitchTo(old);
}

void
ch_binary_append_datetime_seconds(
    ch_binary_insert_handle* h,
    size_t col,
    int64_t seconds,
    bool isnull
) {
    MemoryContext old = MemoryContextSwitchTo(h->cxt);
    icn* node         = resolve_leaf(h, col, isnull);
    uint32_t v        = isnull ? 0 : (uint32_t)seconds;

    dynbuf_append(fixed_data(node), &v, 4);
    MemoryContextSwitchTo(old);
}

void
ch_binary_append_datetime64_raw(
    ch_binary_insert_handle* h,
    size_t col,
    int64_t raw,
    bool isnull
) {
    MemoryContext old = MemoryContextSwitchTo(h->cxt);
    icn* node         = resolve_leaf(h, col, isnull);
    int64_t v         = isnull ? 0 : raw;

    dynbuf_append(fixed_data(node), &v, 8);
    MemoryContextSwitchTo(old);
}

/* Rows committed to a node so far; an array level counts closed subarrays */
static uint64_t
icn_n_rows(const icn* n) {
    switch (n->kind) {
    case ICN_FIXED:
        return n->fixed.elem_size ? n->fixed.data.len / n->fixed.elem_size : 0;
    case ICN_STRING:
        return n->str.offs.len;
    case ICN_NULLABLE:
        return icn_n_rows(n->nullable.inner);
    case ICN_ARRAY:
        return n->array.offs.len;
    case ICN_LC:
        return n->lc.offs.len;
    }
    pg_unreachable();
}

void
ch_binary_array_begin(ch_binary_insert_handle* h, size_t col) {
    icn* node;

    if (h->cursor_len) {
        /*
         * Nested arrays recurse via append_one with col=0, so once an array
         * is open the caller's col is meaningless, descend from cursor.
         */
        node = h->cursor[h->cursor_len - 1]->array.values;
    } else {
        if (col >= h->ncols) {
            ereport(
                ERROR,
                errcode(ERRCODE_FDW_ERROR),
                errmsg("pg_clickhouse: array_begin: col out of range")
            );
        }
        node = h->cols[col].root;
    }

    MemoryContext old = MemoryContextSwitchTo(h->cxt);

    if (node->kind == ICN_NULLABLE && node->nullable.inner->kind == ICN_ARRAY) {
        /* Nullable(Array(...)): the array value itself is non-null */
        uint8_t b = 0;

        dynbuf_append(&node->nullable.null_map, &b, 1);
        node = node->nullable.inner;
    }
    if (node->kind != ICN_ARRAY) {
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_ERROR),
            errmsg("pg_clickhouse: array_begin: column is not Array")
        );
    }
    if (h->cursor_len == h->cursor_cap) {
        h->cursor_cap = h->cursor_cap ? h->cursor_cap * 2 : 4;
        h->cursor     = h->cursor ? repalloc(h->cursor, h->cursor_cap * sizeof(icn*))
                                  : palloc(h->cursor_cap * sizeof(icn*));
    }
    if (h->cursor_len == 0) {
        h->cursor_col = col;
    }
    h->cursor[h->cursor_len++] = node;
    MemoryContextSwitchTo(old);
}

void
ch_binary_array_end(ch_binary_insert_handle* h) {
    if (h->cursor_len == 0) {
        return;
    }
    icn* a            = h->cursor[--h->cursor_len];
    MemoryContext old = MemoryContextSwitchTo(h->cxt);

    u64buf_push(&a->array.offs, icn_n_rows(a->array.values));
    MemoryContextSwitchTo(old);
}

bool
ch_binary_array_active(const ch_binary_insert_handle* h) {
    return h && h->cursor_len > 0;
}

chc_kind
ch_binary_column_kind(const ch_binary_insert_handle* h, size_t col) {
    if (col >= h->ncols) {
        return CHC_VOID;
    }

    /*
     * While nested, surface CHC_ARRAY until the innermost layer is open;
     * at that point return the leaf kind so encode targets scalars.
     */
    const icn* node = cursor_node(h, col);

    if (node->kind == ICN_NULLABLE) {
        node = node->nullable.inner;
    }
    switch (node->kind) {
    case ICN_ARRAY:
        return CHC_ARRAY;
    case ICN_LC:
        return CHC_STRING; /* PG side targets TEXT */
    default:
        return chc_type_kind(node->type);
    }
}

uint32_t
ch_binary_column_datetime64_precision(const ch_binary_insert_handle* h, size_t col) {
    if (col >= h->ncols) {
        return 0;
    }
    const icn* node = cursor_node(h, col);

    for (;;) {
        if (node->kind == ICN_NULLABLE) {
            node = node->nullable.inner;
        } else if (node->kind == ICN_ARRAY) {
            node = node->array.values;
        } else {
            break;
        }
    }
    return node->kind == ICN_FIXED ? node->fixed.dt64_precision : 0;
}

/* Dedup map for LowCardinality dict */
typedef struct lcd_key {
    const uint8_t* bytes;
    size_t len;
} lcd_key;

typedef struct lcd_entry {
    uint32 status;
    lcd_key key;
    uint32 idx;
} lcd_entry;

#define SH_PREFIX lcd
#define SH_ELEMENT_TYPE lcd_entry
#define SH_KEY_TYPE lcd_key
#define SH_KEY key
#define SH_HASH_KEY(tb, key) hash_bytes((key).bytes, (int)(key).len)
#define SH_EQUAL(tb, a, b)                                                             \
    ((a).len == (b).len && memcmp((a).bytes, (b).bytes, (a).len) == 0)
#define SH_SCOPE static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

/* Build LC dict (collect unique strings in insertion order) */
static void
build_lc_dict(
    const icn* node,
    uint64_t** out_dict_offs,
    uint8_t** out_dict_data,
    size_t* out_dict_n,
    void** out_keys,
    int* out_key_size,
    size_t* out_n_rows
) {
    bool nullable       = node->lc.inner_nullable;
    size_t n_rows       = node->lc.offs.len;
    uint64_t* dict_offs = NULL;
    uint8_t* dict_data  = NULL;
    uint32_t* keys      = n_rows ? palloc(n_rows * sizeof(uint32_t)) : NULL;
    size_t dict_n       = 0;
    size_t dict_cap     = 0;
    size_t data_len     = 0;
    lcd_hash* ht =
        n_rows
            ? lcd_create(
                  CurrentMemoryContext, (uint32)Min(n_rows, (size_t)PG_UINT32_MAX), NULL
              )
            : NULL;

    if (nullable) {
        /* dict[0] = "" sentinel. */
        dict_cap     = 8;
        dict_offs    = palloc(dict_cap * sizeof(uint64_t));
        dict_offs[0] = 0;
        dict_n       = 1;
    }

    for (size_t i = 0; i < n_rows; i++) {
        uint64_t start       = i == 0 ? 0 : node->lc.offs.data[i - 1];
        uint64_t end         = node->lc.offs.data[i];
        size_t len           = (size_t)(end - start);
        const uint8_t* bytes = node->lc.data.data + start;
        lcd_key k            = { bytes, len };
        lcd_entry* entry;
        bool found;

        /* If nullable and this row was null (signalled by null bit), key 0. */
        if (nullable && node->lc.null_map.data[i]) {
            keys[i] = 0;
            continue;
        }

        entry = lcd_insert(ht, k, &found);
        if (found) {
            keys[i] = entry->idx;
            continue;
        }

        if (dict_n == dict_cap) {
            dict_cap  = dict_cap ? dict_cap * 2 : 64;
            dict_offs = dict_offs ? repalloc(dict_offs, dict_cap * sizeof(uint64_t))
                                  : palloc(dict_cap * sizeof(uint64_t));
        }
        data_len += len;
        dict_offs[dict_n] = data_len;
        entry->idx        = (uint32)dict_n;
        keys[i]           = (uint32)dict_n;
        dict_n++;
    }

    if (data_len) {
        lcd_iterator it;
        lcd_entry* e;

        dict_data =
            MemoryContextAllocExtended(CurrentMemoryContext, data_len, MCXT_ALLOC_HUGE);
        lcd_start_iterate(ht, &it);
        while ((e = lcd_iterate(ht, &it)) != NULL) {
            uint64_t s = e->idx == 0 ? 0 : dict_offs[e->idx - 1];

            memcpy(dict_data + s, e->key.bytes, e->key.len);
        }
    }
    if (ht) {
        lcd_destroy(ht);
    }
    *out_dict_offs = dict_offs;
    *out_dict_data = dict_data;
    *out_dict_n    = dict_n;
    *out_keys      = keys;
    *out_key_size  = 4;
    *out_n_rows    = n_rows;
}

static inline chc_column*
col_node(chc_column v) {
    chc_column* n = palloc(sizeof(*n));

    *n = v;
    return n;
}

/*
 * Build chc_column tree in current context, 1:1 with the node tree. Result
 * holds references to nested chc_column objects, buffered column data, and
 * any LowCardinality dictionary. Keep context and column buffers alive
 * until send_data returns.
 */
static chc_column*
finalize_node(icn* n) {
    switch (n->kind) {
    case ICN_FIXED: {
        size_t n_rows = icn_n_rows(n);

        return col_node(
            chc_build_fixed(n->fixed.data.data, n->fixed.elem_size, n_rows)
        );
    }
    case ICN_STRING:
        return col_node(
            chc_build_string(n->str.offs.data, n->str.data.data, n->str.offs.len)
        );
    case ICN_NULLABLE:
        return col_node(chc_build_nullable(
            n->nullable.null_map.data, finalize_node(n->nullable.inner)
        ));
    case ICN_ARRAY:
        return col_node(chc_build_array(
            n->array.offs.data, n->array.offs.len, finalize_node(n->array.values)
        ));
    case ICN_LC: {
        size_t dict_n, n_rows;
        int key_size;
        uint64_t* lc_offs;
        uint8_t* lc_data;
        void* lc_keys;

        build_lc_dict(n, &lc_offs, &lc_data, &dict_n, &lc_keys, &key_size, &n_rows);
        chc_column* dict = col_node(chc_build_string(lc_offs, lc_data, dict_n));

        return col_node(chc_build_lc(key_size, lc_keys, n_rows, dict));
    }
    }
    pg_unreachable();
}

static size_t
node_bytes(const icn* n) {
    switch (n->kind) {
    case ICN_FIXED:
        return n->fixed.data.len;
    case ICN_STRING:
        return n->str.data.len + n->str.offs.len * sizeof(uint64_t);
    case ICN_NULLABLE:
        return n->nullable.null_map.len + node_bytes(n->nullable.inner);
    case ICN_ARRAY:
        return n->array.offs.len * sizeof(uint64_t) + node_bytes(n->array.values);
    case ICN_LC:
        return n->lc.data.len + n->lc.offs.len * sizeof(uint64_t) + n->lc.null_map.len;
    }
    pg_unreachable();
}

static void
reset_node(icn* n) {
    switch (n->kind) {
    case ICN_FIXED:
        dynbuf_reset(&n->fixed.data);
        return;
    case ICN_STRING:
        dynbuf_reset(&n->str.data);
        u64buf_reset(&n->str.offs);
        return;
    case ICN_NULLABLE:
        dynbuf_reset(&n->nullable.null_map);
        reset_node(n->nullable.inner);
        return;
    case ICN_ARRAY:
        u64buf_reset(&n->array.offs);
        reset_node(n->array.values);
        return;
    case ICN_LC:
        dynbuf_reset(&n->lc.data);
        u64buf_reset(&n->lc.offs);
        dynbuf_reset(&n->lc.null_map);
        return;
    }
    pg_unreachable();
}

/*
 * Flush once insert buffered reaches 64MiB, so large COPY or INSERT SELECT
 * streams blocks instead of accumulating all rows in memory. Server coalesces
 * small blocks within one INSERT via min_insert_block_size_rows/bytes.
 */
void
ch_binary_insert_autoflush(ch_binary_insert_state* state) {
    ch_binary_insert_handle* h = state->insert_block;

    if (h) {
        size_t total = 0;
        for (size_t i = 0; i < h->ncols; i++) {
            total += node_bytes(h->cols[i].root);
        }
        if (total >= 64 * 1024 * 1024) {
            ch_binary_flush_block(h);
        }
    }
}

void
ch_binary_flush_block(ch_binary_insert_handle* h) {
    /* Unbalanced array_begin/array_end leaves offsets short of leaf rows */
    Assert(h->cursor_len == 0);

    /*
     * Allocate chc_column objects, block metadata, and LowCardinality dictionaries
     * in per-flush context.
     */
    MemoryContext old  = MemoryContextSwitchTo(h->cxt);
    MemoryContext bcxt = AllocSetContextCreate(
        h->cxt, "pg_clickhouse binary insert flush", ALLOCSET_DEFAULT_SIZES
    );

    MemoryContextSwitchTo(bcxt);

    chc_err err          = {};
    chc_block_col* bcols = h->ncols ? palloc(h->ncols * sizeof(*bcols)) : NULL;
    chc_block_builder bb;

    chc_block_builder_init(&bb, bcols);

    for (size_t i = 0; i < h->ncols; i++) {
        ic_col* c        = &h->cols[i];
        const char* name = c->info.name ? c->info.name : "";
        chc_block_builder_append(&bb, name, strlen(name), c->t, finalize_node(c->root));
    }

    int rc = chc_client_send_data(h->client, &bb, &err);

    if (rc != CHC_OK) {
        if (h->state) {
            h->state->broken = true;
        }
        raise_chc(&err, ERRCODE_FDW_ERROR, "could not insert columns - ");
    }

    MemoryContextSwitchTo(h->cxt);
    MemoryContextDelete(bcxt);

    /* Reset per-column buffers for next batch. */
    for (size_t i = 0; i < h->ncols; i++) {
        reset_node(h->cols[i].root);
    }
    MemoryContextSwitchTo(old);
}

/*
 * Send final empty Data + drain. May ereport on server exception or
 * transport failure. Idempotent via h->finalized. Leaves h->cxt alive;
 * ch_binary_release_insert deletes it.
 */
void
ch_binary_finalize_insert(ch_binary_insert_handle* h) {
    if (!h || h->finalized) {
        return;
    }

    /*
     * Set early so an ereport(ERROR) below still leaves h in the "do not
     * touch the wire" state for the release callback.
     */
    h->finalized = true;

    if (!(h->started && h->client)) {
        return;
    }

    MemoryContext old = MemoryContextSwitchTo(h->cxt);
    char* exc_msg     = NULL;
    bool broke        = false;
    chc_err err       = {};
    int rc            = chc_client_send_data(h->client, NULL, &err);

    if (rc != CHC_OK) {
        broke   = true;
        exc_msg = pstrdup(err.msg[0] ? err.msg : "send_data failed");
    } else {
        /* Drain until EOS or exception. */
        for (;;) {
            chc_packet pkt = {};

            err = (chc_err){ 0 };
            rc  = chc_client_recv_packet(h->client, &pkt, &err);
            if (rc != CHC_OK) {
                broke   = true;
                exc_msg = pstrdup(err.msg[0] ? err.msg : "recv_packet failed");
                chc_packet_clear(h->client, &pkt);
                break;
            }
            if (pkt.kind == CHC_PKT_EXCEPTION) {
                const char* msg = "server exception";

                if (pkt.exception && pkt.exception->display_text) {
                    msg = pkt.exception->display_text;
                } else if (pkt.exception && pkt.exception->name) {
                    msg = pkt.exception->name;
                }
                exc_msg = pstrdup(msg);
                broke   = true;
                chc_packet_clear(h->client, &pkt);
                break;
            }
            chc_packet_clear(h->client, &pkt);
            if (pkt.kind == CHC_PKT_END_OF_STREAM) {
                break;
            }
        }
    }

    /*
     * Server raised mid-INSERT and typically closes the socket; the next op
     * would EPIPE. Mark broken so the cache rebuilds.
     */
    if (broke && h->state) {
        h->state->broken = true;
    }

    MemoryContextSwitchTo(old);

    if (exc_msg) {
        /* exc_msg lives in h->cxt; copy into parent before raising. */
        char* parent_msg = pstrdup(exc_msg);

        ereport(
            ERROR,
            errcode(ERRCODE_FDW_ERROR),
            errmsg("pg_clickhouse: could not finish INSERT - %s", parent_msg)
        );
    }
}

/*
 * Teardown counterpart to finalize. Safe from a MemoryContext reset
 * callback: never raises, never talks to the server. If finalize did not
 * run (mid-query abort), flags the connection broken so it rebuilds on
 * next use.
 */
void
ch_binary_release_insert(ch_binary_insert_handle* h) {
    if (!h) {
        return;
    }

    if (!h->finalized && h->started && h->state) {
        h->state->broken = true;
    }

    MemoryContextDelete(h->cxt);
}
