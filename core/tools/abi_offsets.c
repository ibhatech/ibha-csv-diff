/*
 * abi_offsets.c - reports the wasm32 layout of every public struct.
 *
 * The JS binding reads ibha_csvd_table, ibha_csvd_row and the option structs
 * straight out of linear memory, because the header says so: "an accessor call
 * per cell would cost more than the diff". That means the binding needs byte
 * offsets, and a hand written table of them is a silent correctness hazard. It
 * is not a compile error when a field moves, it is a binding that reads
 * n_columns out of the middle of a pointer.
 *
 * So the offsets are asked of the compiler that will actually build the module,
 * for the target it will actually build for. This translation unit compiles to
 * its own tiny wasm32 module which exports a NUL separated name blob and a
 * parallel array of values; scripts-and-commands/gen_abi.mjs instantiates it and
 * writes js/packages/core/src/abi.ts. The gate regenerates and diffs, so a
 * struct that changes without the binding being regenerated fails the build
 * rather than corrupting a read.
 *
 * It links against nothing: offsetof and sizeof need no engine code, which is
 * why this is a separate module rather than more exports on the engine's.
 */
#include <stddef.h>
#include <stdint.h>

#include "ibha_csvdiff.h"

#define ABI_EXPORT __attribute__((visibility("default")))

/*
 * One list, two expansions, so a name and its value cannot drift apart. Adding a
 * field to a public struct means adding one line here; forgetting to is caught
 * by the binding's own field-count assertion rather than at runtime.
 */
#define ABI_ITEMS(X)                                                                          \
    /* -------------------------------------------------------------- constants -- */        \
    X(const.VERSION_MAJOR, IBHA_CSVD_VERSION_MAJOR)                                           \
    X(const.VERSION_MINOR, IBHA_CSVD_VERSION_MINOR)                                           \
    X(const.VERSION_PATCH, IBHA_CSVD_VERSION_PATCH)                                           \
    X(const.SCHEMA_VERSION, IBHA_CSVD_SCHEMA_VERSION)                                         \
    X(const.DEFAULT_MAX_BYTES, IBHA_CSVD_DEFAULT_MAX_BYTES)                                   \
    X(const.DEFAULT_MAX_CELL_BYTES, IBHA_CSVD_DEFAULT_MAX_CELL_BYTES)                         \
    X(const.NO_ROW, IBHA_CSVD_NO_ROW)                                                         \
    X(const.NO_COLUMN, IBHA_CSVD_NO_COLUMN)                                                   \
    X(const.HEADER_AUTO, IBHA_CSVD_HEADER_AUTO)                                               \
    X(const.HEADER_SCAN_ROWS, IBHA_CSVD_HEADER_SCAN_ROWS)                                     \
    X(const.TYPE_TEXT_MAX, IBHA_CSVD_TYPE_TEXT_MAX)                                           \
    X(const.FIELD_QUOTED, IBHA_CSVD_FIELD_QUOTED)                                             \
    X(const.FIELD_HAS_ESCAPE, IBHA_CSVD_FIELD_HAS_ESCAPE)                                     \
    X(const.FIELD_HAS_NEWLINE, IBHA_CSVD_FIELD_HAS_NEWLINE)                                   \
    X(const.FIELD_EMPTY, IBHA_CSVD_FIELD_EMPTY)                                               \
    X(const.COL_KEY, IBHA_CSVD_COL_KEY)                                                       \
    X(const.COL_REQUIRED, IBHA_CSVD_COL_REQUIRED)                                             \
    X(const.CELL_CHANGED, IBHA_CSVD_CELL_CHANGED)                                             \
    X(const.CELL_SUPPRESSED, IBHA_CSVD_CELL_SUPPRESSED)                                       \
    X(const.CELL_REQUIRED_EMPTY, IBHA_CSVD_CELL_REQUIRED_EMPTY)                               \
    X(const.CELL_TOO_LONG, IBHA_CSVD_CELL_TOO_LONG)                                           \
    X(const.CELL_NOT_NUMERIC, IBHA_CSVD_CELL_NOT_NUMERIC)                                     \
    X(const.CELL_PRECISION, IBHA_CSVD_CELL_PRECISION)                                         \
    X(const.CELL_FINDING, IBHA_CSVD_CELL_FINDING)                                             \
    /* ----------------------------------------------------------------- limits -- */        \
    ABI_STRUCT(X, limits, ibha_csvd_limits)                                                   \
    X(limits.max_bytes, offsetof(ibha_csvd_limits, max_bytes))                                \
    X(limits.max_rows, offsetof(ibha_csvd_limits, max_rows))                                  \
    X(limits.max_columns, offsetof(ibha_csvd_limits, max_columns))                            \
    /* ---------------------------------------------------------- ingest_stats -- */         \
    ABI_STRUCT(X, ingest_stats, ibha_csvd_ingest_stats)                                       \
    X(ingest_stats.bytes, offsetof(ibha_csvd_ingest_stats, bytes))                            \
    X(ingest_stats.line_breaks, offsetof(ibha_csvd_ingest_stats, line_breaks))                \
    X(ingest_stats.quotes, offsetof(ibha_csvd_ingest_stats, quotes))                          \
    /* ---------------------------------------------------------------- dialect -- */        \
    ABI_STRUCT(X, dialect, ibha_csvd_dialect)                                                 \
    X(dialect.delimiter, offsetof(ibha_csvd_dialect, delimiter))                              \
    X(dialect.quote, offsetof(ibha_csvd_dialect, quote))                                      \
    X(dialect.strip_bom, offsetof(ibha_csvd_dialect, strip_bom))                              \
    /* ----------------------------------------------------------- header_opts -- */         \
    ABI_STRUCT(X, header_opts, ibha_csvd_header_opts)                                         \
    X(header_opts.rows, offsetof(ibha_csvd_header_opts, rows))                                \
    X(header_opts.key_row, offsetof(ibha_csvd_header_opts, key_row))                          \
    X(header_opts.required_row, offsetof(ibha_csvd_header_opts, required_row))                \
    X(header_opts.type_row, offsetof(ibha_csvd_header_opts, type_row))                        \
    X(header_opts.name_row, offsetof(ibha_csvd_header_opts, name_row))                        \
    /* ---------------------------------------------------------- compare_opts -- */         \
    ABI_STRUCT(X, compare_opts, ibha_csvd_compare_opts)                                       \
    X(compare_opts.trim_whitespace, offsetof(ibha_csvd_compare_opts, trim_whitespace))        \
    X(compare_opts.char_ignore_pad, offsetof(ibha_csvd_compare_opts, char_ignore_pad))        \
    X(compare_opts.numeric, offsetof(ibha_csvd_compare_opts, numeric))                        \
    X(compare_opts.booleans, offsetof(ibha_csvd_compare_opts, booleans))                      \
    X(compare_opts.date_compare, offsetof(ibha_csvd_compare_opts, date_compare))              \
    X(compare_opts.bool_true, offsetof(ibha_csvd_compare_opts, bool_true))                    \
    X(compare_opts.bool_false, offsetof(ibha_csvd_compare_opts, bool_false))                  \
    X(compare_opts.allow_added_columns, offsetof(ibha_csvd_compare_opts, allow_added_columns)) \
    X(compare_opts.allow_removed_columns,                                                     \
      offsetof(ibha_csvd_compare_opts, allow_removed_columns))                                \
    /* ------------------------------------------------------------ parse_opts -- */         \
    ABI_STRUCT(X, parse_opts, ibha_csvd_parse_opts)                                           \
    X(parse_opts.dialect, offsetof(ibha_csvd_parse_opts, dialect))                            \
    X(parse_opts.header, offsetof(ibha_csvd_parse_opts, header))                              \
    X(parse_opts.compare, offsetof(ibha_csvd_parse_opts, compare))                            \
    X(parse_opts.expect_table, offsetof(ibha_csvd_parse_opts, expect_table))                  \
    X(parse_opts.expect_schema, offsetof(ibha_csvd_parse_opts, expect_schema))                \
    X(parse_opts.size_hint, offsetof(ibha_csvd_parse_opts, size_hint))                        \
    X(parse_opts.hash_rows, offsetof(ibha_csvd_parse_opts, hash_rows))                        \
    /* ----------------------------------------------------------- parse_stats -- */         \
    ABI_STRUCT(X, parse_stats, ibha_csvd_parse_stats)                                         \
    X(parse_stats.bytes, offsetof(ibha_csvd_parse_stats, bytes))                              \
    X(parse_stats.n_rows, offsetof(ibha_csvd_parse_stats, n_rows))                            \
    X(parse_stats.n_fields, offsetof(ibha_csvd_parse_stats, n_fields))                        \
    X(parse_stats.n_columns, offsetof(ibha_csvd_parse_stats, n_columns))                      \
    X(parse_stats.quoted_fields, offsetof(ibha_csvd_parse_stats, quoted_fields))              \
    X(parse_stats.escaped_fields, offsetof(ibha_csvd_parse_stats, escaped_fields))            \
    X(parse_stats.multiline_fields, offsetof(ibha_csvd_parse_stats, multiline_fields))        \
    X(parse_stats.ragged_normalized, offsetof(ibha_csvd_parse_stats, ragged_normalized))      \
    X(parse_stats.blank_lines, offsetof(ibha_csvd_parse_stats, blank_lines))                  \
    /* ------------------------------------------------------------------ table -- */        \
    ABI_STRUCT(X, table, ibha_csvd_table)                                                     \
    X(table.bytes, offsetof(ibha_csvd_table, bytes))                                          \
    X(table.len, offsetof(ibha_csvd_table, len))                                              \
    X(table.field_off, offsetof(ibha_csvd_table, field_off))                                  \
    X(table.field_len, offsetof(ibha_csvd_table, field_len))                                  \
    X(table.field_flags, offsetof(ibha_csvd_table, field_flags))                              \
    X(table.n_fields, offsetof(ibha_csvd_table, n_fields))                                    \
    X(table.row_first_field, offsetof(ibha_csvd_table, row_first_field))                      \
    X(table.n_rows, offsetof(ibha_csvd_table, n_rows))                                        \
    X(table.row_key_hash, offsetof(ibha_csvd_table, row_key_hash))                            \
    X(table.row_full_hash, offsetof(ibha_csvd_table, row_full_hash))                          \
    X(table.row_raw_hash, offsetof(ibha_csvd_table, row_raw_hash))                            \
    X(table.n_columns, offsetof(ibha_csvd_table, n_columns))                                  \
    X(table.quote, offsetof(ibha_csvd_table, quote))                                          \
    X(table.compare_id, offsetof(ibha_csvd_table, compare_id))                                \
    X(table.has_digests, offsetof(ibha_csvd_table, has_digests))                              \
    /* ----------------------------------------------------------------- schema -- */        \
    ABI_STRUCT(X, schema, ibha_csvd_schema)                                                   \
    X(schema.n_columns, offsetof(ibha_csvd_schema, n_columns))                                \
    X(schema.n_key_columns, offsetof(ibha_csvd_schema, n_key_columns))                        \
    X(schema.col_flags, offsetof(ibha_csvd_schema, col_flags))                                \
    X(schema.col_type, offsetof(ibha_csvd_schema, col_type))                                  \
    X(schema.col_size, offsetof(ibha_csvd_schema, col_size))                                  \
    X(schema.col_scale, offsetof(ibha_csvd_schema, col_scale))                                \
    X(schema.key_row, offsetof(ibha_csvd_schema, key_row))                                    \
    X(schema.required_row, offsetof(ibha_csvd_schema, required_row))                          \
    X(schema.type_row, offsetof(ibha_csvd_schema, type_row))                                  \
    X(schema.name_row, offsetof(ibha_csvd_schema, name_row))                                  \
    X(schema.first_data_row, offsetof(ibha_csvd_schema, first_data_row))                      \
    X(schema.names_only, offsetof(ibha_csvd_schema, names_only))                              \
    /* ------------------------------------------------------------- diff_opts -- */         \
    ABI_STRUCT(X, diff_opts, ibha_csvd_diff_opts)                                             \
    X(diff_opts.compare, offsetof(ibha_csvd_diff_opts, compare))                              \
    X(diff_opts.detect_moves, offsetof(ibha_csvd_diff_opts, detect_moves))                    \
    X(diff_opts.source_ordered, offsetof(ibha_csvd_diff_opts, source_ordered))                \
    X(diff_opts.count_suppressed, offsetof(ibha_csvd_diff_opts, count_suppressed))            \
    X(diff_opts.validate, offsetof(ibha_csvd_diff_opts, validate))                            \
    X(diff_opts.require_key, offsetof(ibha_csvd_diff_opts, require_key))                      \
    X(diff_opts.deleted_placement, offsetof(ibha_csvd_diff_opts, deleted_placement))          \
    X(diff_opts.similarity_k, offsetof(ibha_csvd_diff_opts, similarity_k))                    \
    X(diff_opts.similarity_percent, offsetof(ibha_csvd_diff_opts, similarity_percent))        \
    X(diff_opts.max_pair_work, offsetof(ibha_csvd_diff_opts, max_pair_work))                  \
    /* -------------------------------------------------------------------- row -- */        \
    ABI_STRUCT(X, row, ibha_csvd_row)                                                         \
    X(row.kind, offsetof(ibha_csvd_row, kind))                                                \
    X(row.moved, offsetof(ibha_csvd_row, moved))                                              \
    X(row.move_distance, offsetof(ibha_csvd_row, move_distance))                              \
    X(row.source_row, offsetof(ibha_csvd_row, source_row))                                    \
    X(row.target_row, offsetof(ibha_csvd_row, target_row))                                    \
    X(row.n_columns, offsetof(ibha_csvd_row, n_columns))                                      \
    X(row.n_changed_cells, offsetof(ibha_csvd_row, n_changed_cells))                          \
    X(row.n_suppressed_cells, offsetof(ibha_csvd_row, n_suppressed_cells))                    \
    X(row.n_findings, offsetof(ibha_csvd_row, n_findings))                                    \
    X(row.cell_flags, offsetof(ibha_csvd_row, cell_flags))                                    \
    /* ------------------------------------------------------------ diff_stats -- */         \
    ABI_STRUCT(X, diff_stats, ibha_csvd_diff_stats)                                           \
    X(diff_stats.rows_unchanged, offsetof(ibha_csvd_diff_stats, rows_unchanged))              \
    X(diff_stats.rows_modified, offsetof(ibha_csvd_diff_stats, rows_modified))                \
    X(diff_stats.rows_added, offsetof(ibha_csvd_diff_stats, rows_added))                      \
    X(diff_stats.rows_deleted, offsetof(ibha_csvd_diff_stats, rows_deleted))                  \
    X(diff_stats.rows_moved, offsetof(ibha_csvd_diff_stats, rows_moved))                      \
    X(diff_stats.report_rows, offsetof(ibha_csvd_diff_stats, report_rows))                    \
    X(diff_stats.cells_changed, offsetof(ibha_csvd_diff_stats, cells_changed))                \
    X(diff_stats.cells_suppressed, offsetof(ibha_csvd_diff_stats, cells_suppressed))          \
    X(diff_stats.cells_required_empty, offsetof(ibha_csvd_diff_stats, cells_required_empty))  \
    X(diff_stats.cells_too_long, offsetof(ibha_csvd_diff_stats, cells_too_long))              \
    X(diff_stats.cells_not_numeric, offsetof(ibha_csvd_diff_stats, cells_not_numeric))        \
    X(diff_stats.cells_bad_precision, offsetof(ibha_csvd_diff_stats, cells_bad_precision))    \
    X(diff_stats.rows_with_findings, offsetof(ibha_csvd_diff_stats, rows_with_findings))      \
    X(diff_stats.columns_added, offsetof(ibha_csvd_diff_stats, columns_added))                \
    X(diff_stats.columns_removed, offsetof(ibha_csvd_diff_stats, columns_removed))            \
    X(diff_stats.n_columns_compared, offsetof(ibha_csvd_diff_stats, n_columns_compared))      \
    X(diff_stats.paired_by_similarity, offsetof(ibha_csvd_diff_stats, paired_by_similarity))  \
    X(diff_stats.pairing_truncated, offsetof(ibha_csvd_diff_stats, pairing_truncated))        \
    X(diff_stats.all_keys, offsetof(ibha_csvd_diff_stats, all_keys))                          \
    X(diff_stats.moves_forced_off, offsetof(ibha_csvd_diff_stats, moves_forced_off))          \
    /* ---------------------------------------------------------------- segment -- */        \
    ABI_STRUCT(X, segment, ibha_csvd_segment)                                                 \
    X(segment.op, offsetof(ibha_csvd_segment, op))                                            \
    X(segment.start, offsetof(ibha_csvd_segment, start))                                      \
    X(segment.len, offsetof(ibha_csvd_segment, len))                                          \
    /* ------------------------------------------------------------- emit_opts -- */         \
    ABI_STRUCT(X, emit_opts, ibha_csvd_emit_opts)                                             \
    X(emit_opts.format, offsetof(ibha_csvd_emit_opts, format))                                \
    X(emit_opts.changes_only, offsetof(ibha_csvd_emit_opts, changes_only))                    \
    X(emit_opts.include_values, offsetof(ibha_csvd_emit_opts, include_values))                \
    X(emit_opts.cell_diff, offsetof(ibha_csvd_emit_opts, cell_diff))                          \
    X(emit_opts.max_cell_bytes, offsetof(ibha_csvd_emit_opts, max_cell_bytes))                \
    X(emit_opts.max_rows, offsetof(ibha_csvd_emit_opts, max_rows))                            \
    X(emit_opts.csv_formula_guard, offsetof(ibha_csvd_emit_opts, csv_formula_guard))          \
    X(emit_opts.csv_delimiter, offsetof(ibha_csvd_emit_opts, csv_delimiter))                  \
    X(emit_opts.class_prefix, offsetof(ibha_csvd_emit_opts, class_prefix))                    \
    /* ------------------------------------------------------------------- sink -- */        \
    ABI_STRUCT(X, sink, ibha_csvd_sink)                                                       \
    X(sink.write, offsetof(ibha_csvd_sink, write))                                            \
    X(sink.ctx, offsetof(ibha_csvd_sink, ctx))                                                \
    /* ------------------------------------------------------------ buffer_sink -- */        \
    ABI_STRUCT(X, buffer_sink, ibha_csvd_buffer_sink)                                         \
    X(buffer_sink.bytes, offsetof(ibha_csvd_buffer_sink, bytes))                              \
    X(buffer_sink.cap, offsetof(ibha_csvd_buffer_sink, cap))                                  \
    X(buffer_sink.len, offsetof(ibha_csvd_buffer_sink, len))                                  \
    X(buffer_sink.overflow, offsetof(ibha_csvd_buffer_sink, overflow))                        \
    /* ---------------------------------------------------------- buffer_reader -- */        \
    ABI_STRUCT(X, buffer_reader, ibha_csvd_buffer_reader)                                     \
    X(buffer_reader.bytes, offsetof(ibha_csvd_buffer_reader, bytes))                          \
    X(buffer_reader.len, offsetof(ibha_csvd_buffer_reader, len))                              \
    X(buffer_reader.pos, offsetof(ibha_csvd_buffer_reader, pos))

/* The size and the alignment travel with every struct, because the binding
 * allocates them and a wrong size is the same class of bug as a wrong offset. */
#define ABI_STRUCT(X, name, type) \
    X(name.__size, sizeof(type))  \
    X(name.__align, _Alignof(type))

#define ABI_NAME(n, v) #n "\0"
#define ABI_VALUE(n, v) (uint32_t)(v),

static const char ABI_NAMES[] = ABI_ITEMS(ABI_NAME);
static const uint32_t ABI_VALUES[] = {ABI_ITEMS(ABI_VALUE)};

ABI_EXPORT const char *abi_names(void);
ABI_EXPORT const uint32_t *abi_values(void);
ABI_EXPORT uint32_t abi_count(void);
ABI_EXPORT uint32_t abi_names_len(void);
ABI_EXPORT uint32_t abi_pointer_size(void);

const char *abi_names(void) { return ABI_NAMES; }
const uint32_t *abi_values(void) { return ABI_VALUES; }
uint32_t abi_count(void) { return (uint32_t)(sizeof(ABI_VALUES) / sizeof(ABI_VALUES[0])); }
uint32_t abi_names_len(void) { return (uint32_t)sizeof(ABI_NAMES); }

/* Asserted by the binding rather than assumed. wasm64 would change every
 * pointer field's offset and every pointer read's width, and the failure would
 * be a plausible looking wrong number rather than a crash. */
uint32_t abi_pointer_size(void) { return (uint32_t)sizeof(void *); }
