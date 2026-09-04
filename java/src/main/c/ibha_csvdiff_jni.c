/*
 * JNI glue for com.ibhatech.csvdiff.
 *
 * Two rules this file obeys, and the second is the one that shapes it.
 *
 * **It contains no diff semantics.** Not a comparison, not a normalization, not a
 * decision about what a row is. It moves bytes and pointers across the boundary
 * and nothing else. Everything else lives in the C core, once, per spec 9.
 *
 * **Nothing crosses the boundary per cell.** A 15 MB pair is 1.8 million cells, and
 * the public header's note on ibha_csvd_table says an accessor call per cell would
 * cost more than the diff. So:
 *
 *   - The columnar arrays of ibha_csvd_table are handed over once, as direct
 *     ByteBuffers over engine memory. Java decodes cells out of them itself, which
 *     is what the note on ibha_csvd_table in the public header asks bindings to do.
 *   - The report cursor is drained a batch of rows at a time into a caller supplied
 *     direct buffer, default 1,000 rows, rather than one call per row. Measured,
 *     this one is small: at 147,000 report rows the difference between a crossing
 *     per row and a crossing per thousand is inside the noise. It is kept because
 *     it costs nothing and because the count it bounds is rows, not cells.
 *   - Input bytes arrive in a direct ByteBuffer, one call per batch, which is the
 *     row feed decision of spec 13.6 applied to the direction bytes actually
 *     travel here.
 *
 * One thing this binding does *not* need, and the JS binding does: generated struct
 * offsets. A wasm host has no C compiler at the boundary, so `gen_abi.mjs` reports
 * the layout of every public struct and the JS side reads fields by offset. Here
 * the boundary *is* C, so a struct field is a struct field. That removes a whole
 * class of drift, and it is why there is no Java equivalent of `abi.ts`.
 *
 * Strings: everything that could be non-ASCII crosses as byte[] holding real
 * UTF-8, never as String. JNI's GetStringUTFChars produces *modified* UTF-8, which
 * encodes NUL as two bytes and astral characters as surrogate pairs, and handing
 * that to a C library expecting UTF-8 is a silent corruption of exactly the data a
 * diff is meant to be trusted about.
 */
#include <jni.h>
#include <stdlib.h>
#include <string.h>

#include "ibha_csvdiff.h"

/*
 * Generated at build time by `javac -h` from NativeEngine.java, and included
 * rather than merely produced: it declares a prototype for every native method,
 * so the C compiler checks each definition below against the Java declaration. A
 * parameter added, removed or retyped on either side fails the build here instead
 * of becoming an UnsatisfiedLinkError, or worse a silently misread argument, at
 * run time. It is also what satisfies -Wmissing-prototypes.
 */
#include "com_ibhatech_csvdiff_jni_NativeEngine.h"

/* Handles cross as jlong. A jlong holds any pointer on every platform this ships
 * to, and the alternative, a ByteBuffer wrapping the pointer, costs an object per
 * handle for no added safety: a stale jlong and a stale wrapper are equally
 * stale. Java keeps them private and non-null-checked in exactly one class. */
#define PTR(x) ((void *)(intptr_t)(x))
#define HANDLE(p) ((jlong)(intptr_t)(p))

/* ------------------------------------------------------------- row records -- */

/*
 * One report row as the batch drain writes it. Fixed 32 byte head, then one flag
 * byte per compared column, padded so the next record starts 4 byte aligned.
 *
 * Row numbers are converted to -1 for absent here rather than travelling as
 * 0xFFFFFFFF, because Java has no unsigned int and a sentinel that reads as
 * -1 by accident is worse than one that means it.
 *
 * The stride is computed here and asked for by Java rather than computed on both
 * sides, so the two cannot disagree about where the second row starts.
 */
#define ROW_RECORD_HEAD 32

static jint row_record_stride(jint n_columns) {
    return (jint)(ROW_RECORD_HEAD + ((n_columns + 3) & ~3));
}

JNIEXPORT jint JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_rowRecordStride(JNIEnv *env, jclass cls, jint n_columns) {
    (void)env;
    (void)cls;
    return row_record_stride(n_columns);
}

/* ---------------------------------------------------------------- version -- */

JNIEXPORT void JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_version(JNIEnv *env, jclass cls, jintArray out) {
    (void)cls;
    int v[3] = {0, 0, 0};
    ibha_csvd_version(&v[0], &v[1], &v[2]);
    (*env)->SetIntArrayRegion(env, out, 0, 3, (const jint *)v);
}

JNIEXPORT jstring JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_statusName(JNIEnv *env, jclass cls, jint status) {
    (void)cls;
    return (*env)->NewStringUTF(env, ibha_csvd_status_name((ibha_csvd_status)status));
}

JNIEXPORT jint JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_schemaVersion(JNIEnv *env, jclass cls) {
    (void)env;
    (void)cls;
    return (jint)IBHA_CSVD_SCHEMA_VERSION;
}

/* ---------------------------------------------------------------- context -- */

JNIEXPORT jlong JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_ctxNew(JNIEnv *env, jclass cls, jlong max_bytes,
                                                  jint max_rows, jint max_columns) {
    (void)env;
    (void)cls;
    ibha_csvd_limits lim;
    ibha_csvd_limits_init(&lim);
    if (max_bytes > 0) lim.max_bytes = (uint64_t)max_bytes;
    if (max_rows > 0) lim.max_rows = (uint32_t)max_rows;
    if (max_columns > 0) lim.max_columns = (uint32_t)max_columns;
    return HANDLE(ibha_csvd_ctx_new(&lim));
}

JNIEXPORT void JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_ctxFree(JNIEnv *env, jclass cls, jlong ctx) {
    (void)env;
    (void)cls;
    ibha_csvd_ctx_free((ibha_csvd_ctx *)PTR(ctx));
}

JNIEXPORT jint JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_ctxStatus(JNIEnv *env, jclass cls, jlong ctx) {
    (void)env;
    (void)cls;
    return (jint)ibha_csvd_ctx_status((const ibha_csvd_ctx *)PTR(ctx));
}

/*
 * The error detail, as bytes rather than as a String.
 *
 * NewStringUTF takes *modified* UTF-8, and this message is not: it names the
 * offending key, which is file data, which can be any UTF-8 at all. A four byte
 * sequence handed to NewStringUTF is undefined behaviour in the JVM, and the one
 * place that would bite is the error message for a duplicate key containing an
 * emoji or a CJK ideograph, which is exactly when a caller most needs to read it.
 */
JNIEXPORT jbyteArray JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_ctxError(JNIEnv *env, jclass cls, jlong ctx) {
    (void)cls;
    const char *msg = ibha_csvd_ctx_error((const ibha_csvd_ctx *)PTR(ctx));
    if (!msg) msg = "";
    jsize n = (jsize)strlen(msg);
    jbyteArray out = (*env)->NewByteArray(env, n);
    if (out) (*env)->SetByteArrayRegion(env, out, 0, n, (const jbyte *)msg);
    return out;
}

JNIEXPORT jlong JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_ctxBytesReserved(JNIEnv *env, jclass cls, jlong ctx) {
    (void)env;
    (void)cls;
    return (jlong)ibha_csvd_ctx_bytes_reserved((const ibha_csvd_ctx *)PTR(ctx));
}

/* -------------------------------------------------------- comparison opts -- */

/*
 * The comparison settings, built once and handed to both parses and to the diff.
 *
 * They are a native allocation rather than a bag of parameters repeated at each
 * call site because the engine bakes them into every row digest and refuses a pair
 * whose compare_id differs. Building them once makes that agreement structural
 * instead of something three call sites have to remember, and it gives the two
 * boolean word lists, which the engine borrows rather than copies, one owner with
 * one lifetime.
 */
typedef struct {
    ibha_csvd_compare_opts opts;
    char *bool_true;
    char *bool_false;
} jni_compare;

static char *dup_bytes_as_cstring(JNIEnv *env, jbyteArray a) {
    if (!a) return NULL;
    jsize n = (*env)->GetArrayLength(env, a);
    char *s = (char *)malloc((size_t)n + 1);
    if (!s) return NULL;
    (*env)->GetByteArrayRegion(env, a, 0, n, (jbyte *)s);
    s[n] = '\0';
    return s;
}

JNIEXPORT jlong JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_compareOptsNew(JNIEnv *env, jclass cls, jint trim,
                                                          jint char_ignore_pad, jint numeric,
                                                          jint booleans, jint date_compare,
                                                          jbyteArray bool_true, jbyteArray bool_false,
                                                          jint allow_added, jint allow_removed) {
    (void)cls;
    jni_compare *c = (jni_compare *)calloc(1, sizeof(*c));
    if (!c) return 0;
    ibha_csvd_compare_opts_init(&c->opts);
    c->opts.trim_whitespace = trim;
    c->opts.char_ignore_pad = char_ignore_pad;
    c->opts.numeric = numeric;
    c->opts.booleans = booleans;
    c->opts.date_compare = date_compare;
    c->opts.allow_added_columns = allow_added;
    c->opts.allow_removed_columns = allow_removed;
    c->bool_true = dup_bytes_as_cstring(env, bool_true);
    c->bool_false = dup_bytes_as_cstring(env, bool_false);
    c->opts.bool_true = c->bool_true;
    c->opts.bool_false = c->bool_false;
    return HANDLE(c);
}

JNIEXPORT void JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_compareOptsFree(JNIEnv *env, jclass cls, jlong p) {
    (void)env;
    (void)cls;
    jni_compare *c = (jni_compare *)PTR(p);
    if (!c) return;
    free(c->bool_true);
    free(c->bool_false);
    free(c);
}

/* ------------------------------------------------------------------ parse -- */

JNIEXPORT jlong JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_parseBegin(JNIEnv *env, jclass cls, jlong ctx,
                                                      jlong compare, jint delimiter, jint quote,
                                                      jint strip_bom, jint header_rows, jint key_row,
                                                      jint required_row, jint type_row, jint name_row,
                                                      jlong expect_table, jlong expect_schema,
                                                      jlong size_hint) {
    (void)env;
    (void)cls;
    ibha_csvd_parse_opts o;
    ibha_csvd_parse_opts_init(&o);

    o.dialect.delimiter = (uint8_t)delimiter;
    o.dialect.quote = (uint8_t)quote;
    o.dialect.strip_bom = (uint8_t)strip_bom;

    o.header.rows = (uint32_t)header_rows;
    o.header.key_row = (uint32_t)key_row;
    o.header.required_row = (uint32_t)required_row;
    o.header.type_row = (uint32_t)type_row;
    o.header.name_row = (uint32_t)name_row;

    jni_compare *c = (jni_compare *)PTR(compare);
    if (c) o.compare = c->opts;

    o.expect_table = (const ibha_csvd_table *)PTR(expect_table);
    o.expect_schema = (const ibha_csvd_schema *)PTR(expect_schema);
    if (size_hint > 0) o.size_hint = (uint64_t)size_hint;

    return HANDLE(ibha_csvd_parse_begin((ibha_csvd_ctx *)PTR(ctx), &o));
}

/*
 * One call per batch of input bytes. The buffer is direct, so its address is the
 * address the engine reads from and nothing is copied on the way in; the parser
 * copies it into the context arena itself, which is what makes the caller free to
 * refill and re-send the same buffer.
 */
JNIEXPORT jint JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_parseChunk(JNIEnv *env, jclass cls, jlong parser,
                                                      jobject buf, jint len) {
    (void)cls;
    void *base = (*env)->GetDirectBufferAddress(env, buf);
    if (!base) return (jint)IBHA_CSVD_ERR_INVALID_ARG;
    return (jint)ibha_csvd_parse_chunk((ibha_csvd_parser *)PTR(parser), base, (size_t)len);
}

JNIEXPORT jint JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_parseFinish(JNIEnv *env, jclass cls, jlong parser) {
    (void)env;
    (void)cls;
    return (jint)ibha_csvd_parse_finish((ibha_csvd_parser *)PTR(parser));
}

JNIEXPORT jlong JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_tableOf(JNIEnv *env, jclass cls, jlong parser) {
    (void)env;
    (void)cls;
    return HANDLE(ibha_csvd_table_of((const ibha_csvd_parser *)PTR(parser)));
}

JNIEXPORT jlong JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_schemaOf(JNIEnv *env, jclass cls, jlong parser) {
    (void)env;
    (void)cls;
    return HANDLE(ibha_csvd_schema_of((const ibha_csvd_parser *)PTR(parser)));
}

/* --------------------------------------------------------- columnar views -- */

/*
 * The four arrays a consumer needs to read a cell without calling the engine, plus
 * the row offsets that turn a (row, column) into a field index. Handed over once
 * per table as direct ByteBuffers over engine memory: no copy, and no per cell
 * call.
 *
 * They are valid until the context is freed. Java holds them behind a handle that
 * refuses to hand out a cell after dispose, because the alternative to that check
 * is a use after free that reads plausible looking bytes.
 */
JNIEXPORT jobject JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_tableBytes(JNIEnv *env, jclass cls, jlong table) {
    (void)cls;
    const ibha_csvd_table *t = (const ibha_csvd_table *)PTR(table);
    if (!t || !t->bytes) return NULL;
    return (*env)->NewDirectByteBuffer(env, (void *)(intptr_t)t->bytes, (jlong)t->len);
}

JNIEXPORT jobject JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_tableFieldOff(JNIEnv *env, jclass cls, jlong table) {
    (void)cls;
    const ibha_csvd_table *t = (const ibha_csvd_table *)PTR(table);
    if (!t || !t->field_off) return NULL;
    return (*env)->NewDirectByteBuffer(env, t->field_off, (jlong)t->n_fields * 4);
}

JNIEXPORT jobject JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_tableFieldLen(JNIEnv *env, jclass cls, jlong table) {
    (void)cls;
    const ibha_csvd_table *t = (const ibha_csvd_table *)PTR(table);
    if (!t || !t->field_len) return NULL;
    return (*env)->NewDirectByteBuffer(env, t->field_len, (jlong)t->n_fields * 4);
}

JNIEXPORT jobject JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_tableFieldFlags(JNIEnv *env, jclass cls, jlong table) {
    (void)cls;
    const ibha_csvd_table *t = (const ibha_csvd_table *)PTR(table);
    if (!t || !t->field_flags) return NULL;
    return (*env)->NewDirectByteBuffer(env, t->field_flags, (jlong)t->n_fields);
}

JNIEXPORT jobject JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_tableRowFirstField(JNIEnv *env, jclass cls, jlong table) {
    (void)cls;
    const ibha_csvd_table *t = (const ibha_csvd_table *)PTR(table);
    if (!t || !t->row_first_field) return NULL;
    /* n_rows + 1 entries: the last is the sentinel that makes a ragged last row
     * representable rather than an index calculation nobody bounds checks. */
    return (*env)->NewDirectByteBuffer(env, t->row_first_field, ((jlong)t->n_rows + 1) * 4);
}

/* n_rows, n_columns, n_fields, byte length, quote byte. */
JNIEXPORT void JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_tableDims(JNIEnv *env, jclass cls, jlong table,
                                                     jintArray out) {
    (void)cls;
    const ibha_csvd_table *t = (const ibha_csvd_table *)PTR(table);
    jint v[5] = {0, 0, 0, 0, 0};
    if (t) {
        v[0] = (jint)t->n_rows;
        v[1] = (jint)t->n_columns;
        v[2] = (jint)t->n_fields;
        v[3] = (jint)t->len;
        v[4] = (jint)t->quote;
    }
    (*env)->SetIntArrayRegion(env, out, 0, 5, v);
}

/*
 * The escaped path, and the only per cell call in the binding.
 *
 * A field's byte range *is* its logical value unless the field carries a ""
 * pair, so Java reads the range directly and only falls back to here when the
 * escape flag is set. The engine counts escaped fields separately precisely
 * because they are rare; on the p90 pair they are a fraction of a percent.
 */
JNIEXPORT jint JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_fieldCopy(JNIEnv *env, jclass cls, jlong table, jint field,
                                                     jobject dst, jint cap) {
    (void)cls;
    const ibha_csvd_table *t = (const ibha_csvd_table *)PTR(table);
    void *base = (*env)->GetDirectBufferAddress(env, dst);
    if (!t || !base) return (jint)IBHA_CSVD_ERR_INVALID_ARG;
    return (jint)ibha_csvd_field_copy(t, (uint32_t)field, (uint8_t *)base, (size_t)cap);
}

JNIEXPORT jint JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_fieldLogicalLen(JNIEnv *env, jclass cls, jlong table,
                                                           jint field) {
    (void)env;
    (void)cls;
    return (jint)ibha_csvd_field_logical_len((const ibha_csvd_table *)PTR(table), (uint32_t)field);
}

/* ----------------------------------------------------------------- schema -- */

/* n_columns, n_key_columns, key_row, required_row, type_row, name_row,
 * first_data_row, names_only. Absent rows arrive as -1 rather than as
 * IBHA_CSVD_NO_ROW, for the reason given on the row records. */
JNIEXPORT void JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_schemaInts(JNIEnv *env, jclass cls, jlong schema,
                                                      jintArray out) {
    (void)cls;
    const ibha_csvd_schema *s = (const ibha_csvd_schema *)PTR(schema);
    jint v[8] = {0, 0, -1, -1, -1, -1, 0, 0};
    if (s) {
        v[0] = (jint)s->n_columns;
        v[1] = (jint)s->n_key_columns;
        v[2] = s->key_row == IBHA_CSVD_NO_ROW ? -1 : (jint)s->key_row;
        v[3] = s->required_row == IBHA_CSVD_NO_ROW ? -1 : (jint)s->required_row;
        v[4] = s->type_row == IBHA_CSVD_NO_ROW ? -1 : (jint)s->type_row;
        v[5] = s->name_row == IBHA_CSVD_NO_ROW ? -1 : (jint)s->name_row;
        v[6] = (jint)s->first_data_row;
        v[7] = (jint)s->names_only;
    }
    (*env)->SetIntArrayRegion(env, out, 0, 8, v);
}

/* col_flags, col_type, col_size and col_scale for the compared columns. Copied
 * rather than mapped: they are a handful of bytes per column, they are read once
 * when the diff opens, and a copy has no lifetime question attached to it. */
JNIEXPORT void JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_schemaColumns(JNIEnv *env, jclass cls, jlong schema,
                                                         jint n, jbyteArray flags, jbyteArray types,
                                                         jintArray sizes, jintArray scales) {
    (void)cls;
    const ibha_csvd_schema *s = (const ibha_csvd_schema *)PTR(schema);
    if (!s || n <= 0) return;
    if (n > (jint)s->n_columns) n = (jint)s->n_columns;
    if (s->col_flags) (*env)->SetByteArrayRegion(env, flags, 0, n, (const jbyte *)s->col_flags);
    if (s->col_type) (*env)->SetByteArrayRegion(env, types, 0, n, (const jbyte *)s->col_type);
    if (s->col_size) (*env)->SetIntArrayRegion(env, sizes, 0, n, (const jint *)s->col_size);
    if (s->col_scale) (*env)->SetIntArrayRegion(env, scales, 0, n, (const jint *)s->col_scale);
}

/* ------------------------------------------------------------------- diff -- */

JNIEXPORT jlong JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_diffRun(JNIEnv *env, jclass cls, jlong ctx, jlong compare,
                                                   jlong src_table, jlong src_schema, jlong tgt_table,
                                                   jlong tgt_schema, jint detect_moves,
                                                   jint source_ordered, jint count_suppressed,
                                                   jint validate, jint require_key,
                                                   jint deleted_placement, jint similarity_k,
                                                   jint similarity_percent) {
    (void)env;
    (void)cls;
    ibha_csvd_diff_opts o;
    ibha_csvd_diff_opts_init(&o);

    jni_compare *c = (jni_compare *)PTR(compare);
    if (c) o.compare = c->opts;

    o.detect_moves = detect_moves;
    o.source_ordered = source_ordered;
    o.count_suppressed = count_suppressed;
    o.validate = validate;
    o.require_key = require_key;
    o.deleted_placement = (uint8_t)deleted_placement;
    if (similarity_k > 0) o.similarity_k = (uint32_t)similarity_k;
    if (similarity_percent > 0) o.similarity_percent = (uint32_t)similarity_percent;

    return HANDLE(ibha_csvd_diff_run((ibha_csvd_ctx *)PTR(ctx), (const ibha_csvd_table *)PTR(src_table),
                                     (const ibha_csvd_schema *)PTR(src_schema),
                                     (const ibha_csvd_table *)PTR(tgt_table),
                                     (const ibha_csvd_schema *)PTR(tgt_schema), &o));
}

JNIEXPORT jlong JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_diffTable(JNIEnv *env, jclass cls, jlong diff, jint side) {
    (void)env;
    (void)cls;
    return HANDLE(ibha_csvd_diff_table((const ibha_csvd_diff *)PTR(diff), (ibha_csvd_side)side));
}

JNIEXPORT jlong JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_diffSchema(JNIEnv *env, jclass cls, jlong diff, jint side) {
    (void)env;
    (void)cls;
    return HANDLE(ibha_csvd_diff_schema((const ibha_csvd_diff *)PTR(diff), (ibha_csvd_side)side));
}

JNIEXPORT jint JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_diffColumns(JNIEnv *env, jclass cls, jlong diff) {
    (void)env;
    (void)cls;
    return (jint)ibha_csvd_diff_columns((const ibha_csvd_diff *)PTR(diff));
}

/* The row level counters, which are final the moment diff_run returns. The cell
 * level ones are deliberately not exposed: they accumulate as a cursor advances,
 * so a struct read gives a number that depends on how often the caller happened to
 * drain. Java takes those from the summary emitter, which zeroes them and drains
 * its own cursor, exactly as the JS binding does and for the same reason. */
JNIEXPORT void JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_diffRowStats(JNIEnv *env, jclass cls, jlong diff,
                                                        jintArray out) {
    (void)cls;
    const ibha_csvd_diff_stats *s = ibha_csvd_diff_stats_of((const ibha_csvd_diff *)PTR(diff));
    jint v[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (s) {
        v[0] = (jint)s->rows_unchanged;
        v[1] = (jint)s->rows_modified;
        v[2] = (jint)s->rows_added;
        v[3] = (jint)s->rows_deleted;
        v[4] = (jint)s->rows_moved;
        v[5] = (jint)s->report_rows;
        v[6] = (jint)s->columns_added;
        v[7] = (jint)s->columns_removed;
        v[8] = (jint)s->n_columns_compared;
        v[9] = (jint)s->paired_by_similarity;
        v[10] = (jint)s->all_keys;
        v[11] = (jint)s->moves_forced_off;
    }
    (*env)->SetIntArrayRegion(env, out, 0, 12, v);
}

/* ----------------------------------------------------------------- cursor -- */

JNIEXPORT jlong JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_cursorOpen(JNIEnv *env, jclass cls, jlong diff) {
    (void)env;
    (void)cls;
    return HANDLE(ibha_csvd_cursor_open((ibha_csvd_diff *)PTR(diff)));
}

/*
 * Drains up to max_rows report rows into the caller's direct buffer.
 *
 * This is the batch that spec 13.6 argues for, in the direction rows actually
 * travel. Copying the fixed fields plus the flag bytes is what makes a row outlive
 * the cursor position that produced it; cell *values* are not copied, because the
 * table bytes are immutable for the life of the context and Java reads them
 * straight out of the columnar arrays whenever it decodes.
 *
 * On the batch size itself, honestly: benchmarking the same walk at 1,000 rows a
 * call and at 1 row a call puts the difference inside the noise on a 147,000 row
 * report. What the batch actually buys is that the crossing count scales with rows
 * rather than with cells, and it is the cell count that the header warns about.
 *
 * Returns the number of rows written, 0 at end of stream, or a negative status.
 * No filtering happens here: changes-only is a policy and this file holds none.
 */
JNIEXPORT jint JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_cursorNextBatch(JNIEnv *env, jclass cls, jlong cursor,
                                                           jobject out, jint max_rows,
                                                           jint n_columns) {
    (void)cls;
    ibha_csvd_cursor *cur = (ibha_csvd_cursor *)PTR(cursor);
    uint8_t *base = (uint8_t *)(*env)->GetDirectBufferAddress(env, out);
    if (!cur || !base) return (jint)IBHA_CSVD_ERR_INVALID_ARG;

    const jint stride = row_record_stride(n_columns);
    jint written = 0;

    while (written < max_rows) {
        int more = ibha_csvd_cursor_next(cur);
        if (more == 0) break;
        if (more < 0) return (jint)more;

        const ibha_csvd_row *r = ibha_csvd_cursor_row(cur);
        if (!r) break;

        uint8_t *rec = base + (size_t)written * (size_t)stride;
        int32_t head[8];
        head[0] = (int32_t)r->kind;
        head[1] = (int32_t)r->moved;
        head[2] = r->move_distance;
        head[3] = r->source_row == IBHA_CSVD_NO_ROW ? -1 : (int32_t)r->source_row;
        head[4] = r->target_row == IBHA_CSVD_NO_ROW ? -1 : (int32_t)r->target_row;
        head[5] = (int32_t)r->n_changed_cells;
        head[6] = (int32_t)r->n_suppressed_cells;
        head[7] = (int32_t)r->n_findings;
        memcpy(rec, head, sizeof head);

        uint32_t n = r->n_columns < (uint32_t)n_columns ? r->n_columns : (uint32_t)n_columns;
        if (r->cell_flags && n) memcpy(rec + ROW_RECORD_HEAD, r->cell_flags, n);
        if (n < (uint32_t)n_columns) memset(rec + ROW_RECORD_HEAD + n, 0, (size_t)n_columns - n);

        written++;
    }
    return written;
}

/* --------------------------------------------------------------- segments -- */

/*
 * Intra cell segments for one column of one report row, per spec 7.
 *
 * The row is identified by its two row indices rather than by a live cursor
 * position, because the engine reads only those two fields off the row it is
 * handed. That is what lets a consumer ask for segments on a row it has already
 * walked past, which is the normal case: a reviewer clicks a cell.
 */
JNIEXPORT jint JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_cellSegments(JNIEnv *env, jclass cls, jlong diff,
                                                        jint source_row, jint target_row, jint col,
                                                        jint mode, jint max_bytes, jobject out,
                                                        jint cap) {
    (void)cls;
    ibha_csvd_row row;
    memset(&row, 0, sizeof row);
    row.source_row = source_row < 0 ? IBHA_CSVD_NO_ROW : (uint32_t)source_row;
    row.target_row = target_row < 0 ? IBHA_CSVD_NO_ROW : (uint32_t)target_row;

    ibha_csvd_segment *dst = NULL;
    if (out) dst = (ibha_csvd_segment *)(*env)->GetDirectBufferAddress(env, out);

    return (jint)ibha_csvd_cell_segments((ibha_csvd_diff *)PTR(diff), &row, (uint32_t)col,
                                         (ibha_csvd_cell_diff_mode)mode, (uint32_t)max_bytes, dst,
                                         (uint32_t)cap);
}

/* ------------------------------------------------------------------- emit -- */

/*
 * Runs an emitter into the caller's direct buffer and reports how many bytes the
 * report is.
 *
 * Passing a null buffer measures without writing, because the buffer sink counts
 * past the end rather than truncating. So the Java side sizes with one pass and
 * fills with a second, which is what the JS binding does and carries the same
 * limitation: ibha_csvd_emit drains the whole diff in one call, so a large report
 * is produced twice. A resumable emitter is the fix and it belongs in the engine.
 *
 * Returns the byte length of the report, or a negative status.
 */
JNIEXPORT jlong JNICALL
Java_com_ibhatech_csvdiff_jni_NativeEngine_emit(JNIEnv *env, jclass cls, jlong diff, jint format,
                                                jint changes_only, jint include_values, jint cell_diff,
                                                jint max_cell_bytes, jint max_rows,
                                                jint csv_formula_guard, jint csv_delimiter,
                                                jbyteArray class_prefix, jobject out, jint cap) {
    (void)cls;
    ibha_csvd_emit_opts o;
    ibha_csvd_emit_opts_init(&o, (ibha_csvd_emit_format)format);
    o.changes_only = changes_only;
    o.include_values = include_values;
    o.cell_diff = (uint8_t)cell_diff;
    o.max_cell_bytes = (uint32_t)max_cell_bytes;
    o.max_rows = (uint32_t)max_rows;
    o.csv_formula_guard = csv_formula_guard;
    if (csv_delimiter > 0) o.csv_delimiter = (uint8_t)csv_delimiter;

    char *prefix = dup_bytes_as_cstring(env, class_prefix);
    if (prefix) o.class_prefix = prefix;

    void *base = NULL;
    if (out) base = (*env)->GetDirectBufferAddress(env, out);

    ibha_csvd_buffer_sink bs;
    ibha_csvd_buffer_sink_init(&bs, base, base ? (size_t)cap : 0);
    ibha_csvd_sink sink;
    ibha_csvd_buffer_sink_bind(&sink, &bs);

    ibha_csvd_status st = ibha_csvd_emit((ibha_csvd_diff *)PTR(diff), &o, &sink, NULL);
    free(prefix);
    if (st != IBHA_CSVD_OK) return (jlong)st;
    return (jlong)bs.len;
}
