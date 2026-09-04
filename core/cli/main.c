/*
 * ibha-csvdiff - command line front end.
 *
 * Three subcommands. `diff` matches two files and drains the report cursor,
 * reporting counts; it is the Phase 2 end to end path and a driver for the
 * emitters that arrive in Phase 3, not one of them. `parse` runs a single file
 * through the RFC 4180 state machine and reports the index it built, which is
 * the fastest way to see what the parser made of a file someone is complaining
 * about. `ingest` is the Phase 0 byte scan, retained because it is the reference
 * the parser number is read against.
 *
 * Exit codes follow diff(1), per spec 2.6.4, so the tool composes in a shell or
 * a scheduler:
 *     0  inputs identical / operation succeeded
 *     1  differences found
 *     2  error
 */
/* The library is strict C11, and the CLI is built with -std=c11 too, which hides
 * clock_gettime and friends behind glibc's feature test macros. The CLI is the
 * one place that legitimately wants POSIX, so it asks for it explicitly rather
 * than the build loosening the standard for everything. */
#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ibha_csvdiff.h"

#define EXIT_SAME 0
#define EXIT_DIFF 1
#define EXIT_ERROR 2

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void usage(FILE *f) {
    fprintf(f,
            "ibha-csvdiff %d.%d.%d\n"
            "\n"
            "usage:\n"
            "  ibha-csvdiff parse <file> [--header N] [--repeat N] [--no-hash] [--max-mb N]\n"
            "  ibha-csvdiff ingest <file> [--repeat N] [--no-retain] [--max-mb N]\n"
            "  ibha-csvdiff diff <source.csv> <target.csv> [--header N] [--no-moves]\n"
            "                    [--format jsonl|csv|html|summary] [--changes-only]\n"
            "                    [--cell-diff word|char|both] [--max-rows N] [--no-validate]\n"
            "  ibha-csvdiff --version\n"
            "\n"
            "parse options:\n"
            "  --header N     number of header rows: 4 (default), 1 for a names-only\n"
            "                 file, or 0 for a file with no header at all\n"
            "  --repeat N     parse N times and report the best result\n"
            "  --no-hash      skip the per row digests, isolating state machine cost\n"
            "  --max-mb N     override the %llu MB input limit\n"
            "\n"
            "diff options:\n"
            "  --header N     header rows in the source file, default 4\n"
            "  --no-moves     skip move detection\n"
            "  --format F     write the report to stdout in one of the emitter\n"
            "                 formats instead of the human readable counts:\n"
            "                 jsonl (one JSON object per row), csv, html, summary\n"
            "  --changes-only skip rows that are unchanged, unmoved and carry no\n"
            "                 finding. The usual choice for an html report\n"
            "  --cell-diff M  html only: highlight inside a changed cell, where M is\n"
            "                 word, char or both\n"
            "  --max-rows N   stop after N report rows, which is how html output is\n"
            "                 kept bounded\n"
            "  --no-validate  skip the validation findings of spec 13.5\n"
            "  --allow-added-columns    a column in the uploaded file that the source\n"
            "                 does not declare is a finding rather than an error\n"
            "  --allow-removed-columns  a column the source declares that the uploaded\n"
            "                 file does not carry is a finding rather than an error.\n"
            "                 Neither relaxes column order, and a missing KEY column\n"
            "                 is always an error\n"
            "\n"
            "ingest options:\n"
            "  --repeat N     run the ingest N times and report the best result,\n"
            "                 so a cold page cache does not distort the number\n"
            "  --no-retain    scan without retaining bytes, isolating scan cost\n"
            "                 from allocation cost\n"
            "  --max-mb N     override the same limit\n"
            "\n"
            "exit codes: 0 identical, 1 differences found, 2 error\n",
            IBHA_CSVD_VERSION_MAJOR, IBHA_CSVD_VERSION_MINOR, IBHA_CSVD_VERSION_PATCH,
            (unsigned long long)(IBHA_CSVD_DEFAULT_MAX_BYTES / (1024 * 1024)));
}

static int cmd_ingest(int argc, char **argv) {
    const char *path = NULL;
    int repeat = 1;
    int retain = 1;
    uint64_t max_bytes = 0;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            repeat = atoi(argv[++i]);
            if (repeat < 1) repeat = 1;
        } else if (strcmp(argv[i], "--no-retain") == 0) {
            retain = 0;
        } else if (strcmp(argv[i], "--max-mb") == 0 && i + 1 < argc) {
            max_bytes = (uint64_t)strtoull(argv[++i], NULL, 10) * 1024 * 1024;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "ibha-csvdiff: unknown option %s\n", argv[i]);
            return EXIT_ERROR;
        } else if (!path) {
            path = argv[i];
        } else {
            fprintf(stderr, "ibha-csvdiff: unexpected argument %s\n", argv[i]);
            return EXIT_ERROR;
        }
    }

    if (!path) {
        usage(stderr);
        return EXIT_ERROR;
    }

    double best = 0;
    ibha_csvd_ingest_stats stats = {0, 0, 0};
    uint64_t reserved = 0;

    for (int run = 0; run < repeat; run++) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "ibha-csvdiff: cannot open %s\n", path);
            return EXIT_ERROR;
        }

        ibha_csvd_limits lim;
        ibha_csvd_limits_init(&lim);
        if (max_bytes) lim.max_bytes = max_bytes;

        ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(&lim);
        if (!ctx) {
            fprintf(stderr, "ibha-csvdiff: out of memory\n");
            close(fd);
            return EXIT_ERROR;
        }

        ibha_csvd_fd_reader r;
        ibha_csvd_fd_reader_init(&r, fd);

        /* A local file knows its own size, so always supply the hint. Without it
         * retaining N bytes costs about 4x N instead of about 1x. */
        uint64_t hint = 0;
        struct stat sb;
        if (fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_size > 0) {
            hint = (uint64_t)sb.st_size;
        }

        double t0 = now_seconds();
        ibha_csvd_status st = ibha_csvd_ingest(ctx, ibha_csvd_fd_read, &r, retain, hint, &stats);
        double elapsed = now_seconds() - t0;

        if (st != IBHA_CSVD_OK) {
            fprintf(stderr, "ibha-csvdiff: %s: %s\n", ibha_csvd_status_name(st),
                    ibha_csvd_ctx_error(ctx));
            ibha_csvd_ctx_free(ctx);
            close(fd);
            return EXIT_ERROR;
        }

        reserved = ibha_csvd_ctx_bytes_reserved(ctx);
        if (best == 0 || elapsed < best) best = elapsed;

        ibha_csvd_ctx_free(ctx);
        close(fd);
    }

    double mb = (double)stats.bytes / (1024.0 * 1024.0);
    printf("file            %s\n", path);
    printf("bytes           %llu (%.2f MB)\n", (unsigned long long)stats.bytes, mb);
    printf("line breaks     %llu\n", (unsigned long long)stats.line_breaks);
    printf("quotes          %llu\n", (unsigned long long)stats.quotes);
    printf("retained        %s\n", retain ? "yes" : "no");
    printf("arena reserved  %.2f MB (%.2fx input)\n", (double)reserved / (1024.0 * 1024.0),
           stats.bytes ? (double)reserved / (double)stats.bytes : 0.0);
    printf("best of %-7d %.4f s\n", repeat, best);
    printf("throughput      %.0f MB/s\n", best > 0 ? mb / best : 0.0);
    return EXIT_SAME;
}

/*
 * `parse` is the Phase 1 end to end path: a real file, off disk, through the
 * pull reader and the state machine, reporting the index it produced. It is both
 * the throughput harness on real data and the quickest way to see what the
 * parser made of a file that a user is complaining about.
 */
static int cmd_parse(int argc, char **argv) {
    const char *path = NULL;
    int repeat = 1;
    uint32_t header = 4;
    int hash_rows = 1;
    uint64_t max_bytes = 0;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            repeat = atoi(argv[++i]);
            if (repeat < 1) repeat = 1;
        } else if (strcmp(argv[i], "--header") == 0 && i + 1 < argc) {
            header = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--no-hash") == 0) {
            hash_rows = 0;
        } else if (strcmp(argv[i], "--max-mb") == 0 && i + 1 < argc) {
            max_bytes = (uint64_t)strtoull(argv[++i], NULL, 10) * 1024 * 1024;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "ibha-csvdiff: unknown option %s\n", argv[i]);
            return EXIT_ERROR;
        } else if (!path) {
            path = argv[i];
        } else {
            fprintf(stderr, "ibha-csvdiff: unexpected argument %s\n", argv[i]);
            return EXIT_ERROR;
        }
    }
    if (!path) {
        usage(stderr);
        return EXIT_ERROR;
    }

    double best = 0;
    uint64_t reserved = 0, bytes = 0;
    ibha_csvd_parse_stats stats = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    ibha_csvd_schema schema;
    uint32_t n_required = 0;
    memset(&schema, 0, sizeof(schema));

    for (int run = 0; run < repeat; run++) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "ibha-csvdiff: cannot open %s\n", path);
            return EXIT_ERROR;
        }

        ibha_csvd_limits lim;
        ibha_csvd_limits_init(&lim);
        if (max_bytes) lim.max_bytes = max_bytes;

        ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(&lim);
        if (!ctx) {
            fprintf(stderr, "ibha-csvdiff: out of memory\n");
            close(fd);
            return EXIT_ERROR;
        }

        ibha_csvd_parse_opts o;
        ibha_csvd_parse_opts_init(&o);
        o.header.rows = header;
        o.header.key_row = header >= 1 ? 1 : 0;
        o.header.required_row = header >= 2 ? 2 : 0;
        o.header.type_row = header >= 3 ? 3 : 0;
        o.header.name_row = header;
        o.hash_rows = hash_rows;

        /* A local file knows its own size, and the hint is what keeps the index
         * at about 1.4x rather than 2x. */
        struct stat sb;
        if (fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_size > 0) {
            o.size_hint = (uint64_t)sb.st_size;
        }

        ibha_csvd_fd_reader r;
        ibha_csvd_fd_reader_init(&r, fd);

        ibha_csvd_parser *p = NULL;
        double t0 = now_seconds();
        ibha_csvd_status st = ibha_csvd_parse_stream(ctx, ibha_csvd_fd_read, &r, &o, &p);
        double elapsed = now_seconds() - t0;

        if (st != IBHA_CSVD_OK) {
            fprintf(stderr, "ibha-csvdiff: %s: %s\n", ibha_csvd_status_name(st),
                    ibha_csvd_ctx_error(ctx));
            ibha_csvd_ctx_free(ctx);
            close(fd);
            return EXIT_ERROR;
        }

        stats = *ibha_csvd_parse_stats_of(p);
        schema = *ibha_csvd_schema_of(p);
        /* Counted here, while the arena the flags live in is still alive. */
        n_required = 0;
        for (uint32_t c = 0; c < schema.n_columns; c++) {
            if (schema.col_flags[c] & IBHA_CSVD_COL_REQUIRED) n_required++;
        }
        bytes = stats.bytes;
        reserved = ibha_csvd_ctx_bytes_reserved(ctx);
        if (best == 0 || elapsed < best) best = elapsed;

        ibha_csvd_ctx_free(ctx);
        close(fd);
    }

    double mb = (double)bytes / (1024.0 * 1024.0);
    printf("file            %s\n", path);
    printf("bytes           %llu (%.2f MB)\n", (unsigned long long)bytes, mb);
    printf("rows            %u  (%u header, %u data)\n", stats.n_rows, schema.first_data_row,
           stats.n_rows - schema.first_data_row);
    printf("columns         %u  (%u key, %u required)\n", stats.n_columns, schema.n_key_columns,
           n_required);
    printf("cells           %u\n", stats.n_fields);
    printf("quoted cells    %u  (%u escaped, %u multiline)\n", stats.quoted_fields,
           stats.escaped_fields, stats.multiline_fields);
    printf("normalized      %u ragged rows, %u blank lines skipped\n", stats.ragged_normalized,
           stats.blank_lines);
    printf("digests         %s\n", hash_rows ? "computed" : "skipped");
    printf("arena reserved  %.2f MB (%.2fx input)\n", (double)reserved / (1024.0 * 1024.0),
           bytes ? (double)reserved / (double)bytes : 0.0);
    printf("best of %-7d %.4f s\n", repeat, best);
    printf("throughput      %.0f MB/s\n", best > 0 ? mb / best : 0.0);
    return EXIT_SAME;
}

/*
 * `diff` is the Phase 2 end to end path: two real files, matched, with the
 * report pulled through the cursor and counted.
 *
 * It deliberately prints counts and nothing else. The emitters of spec 13.3,
 * jsonl, csv, html and summary, are Phase 3; this is the driver that proves the
 * engine underneath them works on real files and gives diff(1) exit codes so it
 * composes in a shell.
 */
static ibha_csvd_parser *open_side(ibha_csvd_ctx *ctx, const char *path, uint32_t header,
                                   const ibha_csvd_parser *expect,
                                   const ibha_csvd_compare_opts *cmp) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ibha-csvdiff: cannot open %s\n", path);
        return NULL;
    }

    ibha_csvd_parse_opts o;
    ibha_csvd_parse_opts_init(&o);
    /* The comparison settings are baked into every row digest, so both sides must
     * be parsed with the same ones and the diff must be run with them too. */
    if (cmp) o.compare = *cmp;
    if (expect) {
        o.header.rows = IBHA_CSVD_HEADER_AUTO;
        o.expect_table = ibha_csvd_table_of(expect);
        o.expect_schema = ibha_csvd_schema_of(expect);
    } else {
        o.header.rows = header;
        o.header.key_row = header >= 1 ? 1 : 0;
        o.header.required_row = header >= 2 ? 2 : 0;
        o.header.type_row = header >= 3 ? 3 : 0;
        o.header.name_row = header;
    }

    struct stat sb;
    if (fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_size > 0) {
        o.size_hint = (uint64_t)sb.st_size;
    }

    ibha_csvd_fd_reader r;
    ibha_csvd_fd_reader_init(&r, fd);
    ibha_csvd_parser *p = NULL;
    ibha_csvd_status st = ibha_csvd_parse_stream(ctx, ibha_csvd_fd_read, &r, &o, &p);
    close(fd);
    if (st != IBHA_CSVD_OK) return NULL;
    return p;
}

/* --format selects an emitter; without one the CLI keeps printing the counts,
 * which is what a human at a terminal wants and what Phase 2 shipped. */
static int format_from_name(const char *s, ibha_csvd_emit_format *out) {
    if (strcmp(s, "jsonl") == 0) {
        *out = IBHA_CSVD_EMIT_JSONL;
    } else if (strcmp(s, "csv") == 0) {
        *out = IBHA_CSVD_EMIT_CSV;
    } else if (strcmp(s, "html") == 0) {
        *out = IBHA_CSVD_EMIT_HTML;
    } else if (strcmp(s, "summary") == 0) {
        *out = IBHA_CSVD_EMIT_SUMMARY;
    } else {
        return 0;
    }
    return 1;
}

static int cell_diff_from_name(const char *s, uint8_t *out) {
    if (strcmp(s, "word") == 0) {
        *out = IBHA_CSVD_CELLDIFF_WORD;
    } else if (strcmp(s, "char") == 0) {
        *out = IBHA_CSVD_CELLDIFF_CHARACTER;
    } else if (strcmp(s, "both") == 0) {
        *out = IBHA_CSVD_CELLDIFF_WORD_THEN_CHARACTER;
    } else {
        return 0;
    }
    return 1;
}

static int cmd_diff(int argc, char **argv) {
    const char *src_path = NULL, *tgt_path = NULL;
    uint32_t header = 4;
    int no_moves = 0;
    int no_validate = 0;
    int have_format = 0;
    ibha_csvd_emit_format format = IBHA_CSVD_EMIT_JSONL;
    ibha_csvd_emit_opts emit;
    ibha_csvd_compare_opts cmp;

    ibha_csvd_emit_opts_init(&emit, IBHA_CSVD_EMIT_JSONL);
    ibha_csvd_compare_opts_init(&cmp);

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--header") == 0 && i + 1 < argc) {
            header = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--no-moves") == 0) {
            no_moves = 1;
        } else if (strcmp(argv[i], "--no-validate") == 0) {
            no_validate = 1;
        } else if (strcmp(argv[i], "--allow-added-columns") == 0) {
            cmp.allow_added_columns = 1;
        } else if (strcmp(argv[i], "--allow-removed-columns") == 0) {
            cmp.allow_removed_columns = 1;
        } else if (strcmp(argv[i], "--changes-only") == 0) {
            emit.changes_only = 1;
        } else if (strcmp(argv[i], "--max-rows") == 0 && i + 1 < argc) {
            emit.max_rows = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            if (!format_from_name(argv[++i], &format)) {
                fprintf(stderr, "ibha-csvdiff: unknown format %s\n", argv[i]);
                return EXIT_ERROR;
            }
            have_format = 1;
        } else if (strcmp(argv[i], "--cell-diff") == 0 && i + 1 < argc) {
            if (!cell_diff_from_name(argv[++i], &emit.cell_diff)) {
                fprintf(stderr, "ibha-csvdiff: unknown cell diff mode %s\n", argv[i]);
                return EXIT_ERROR;
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "ibha-csvdiff: unknown option %s\n", argv[i]);
            return EXIT_ERROR;
        } else if (!src_path) {
            src_path = argv[i];
        } else if (!tgt_path) {
            tgt_path = argv[i];
        } else {
            fprintf(stderr, "ibha-csvdiff: unexpected argument %s\n", argv[i]);
            return EXIT_ERROR;
        }
    }
    if (!src_path || !tgt_path) {
        usage(stderr);
        return EXIT_ERROR;
    }

    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    if (!ctx) {
        fprintf(stderr, "ibha-csvdiff: out of memory\n");
        return EXIT_ERROR;
    }

    double t0 = now_seconds();
    ibha_csvd_parser *sp = open_side(ctx, src_path, header, NULL, &cmp);
    ibha_csvd_parser *tp = sp ? open_side(ctx, tgt_path, header, sp, &cmp) : NULL;
    double parsed = now_seconds();

    ibha_csvd_diff_opts o;
    ibha_csvd_diff_opts_init(&o);
    o.compare = cmp;
    if (no_moves) o.detect_moves = 0;
    if (no_validate) o.validate = 0;

    ibha_csvd_diff *d = NULL;
    if (sp && tp) {
        d = ibha_csvd_diff_run(ctx, ibha_csvd_table_of(sp), ibha_csvd_schema_of(sp),
                               ibha_csvd_table_of(tp), ibha_csvd_schema_of(tp), &o);
    }
    if (!d) {
        fprintf(stderr, "ibha-csvdiff: %s: %s\n",
                ibha_csvd_status_name(ibha_csvd_ctx_status(ctx)), ibha_csvd_ctx_error(ctx));
        ibha_csvd_ctx_free(ctx);
        return EXIT_ERROR;
    }
    double matched = now_seconds();

    /*
     * With an emitter selected the report goes to stdout and nothing else does,
     * so the tool composes: `ibha-csvdiff diff a b --format jsonl | jq`. The
     * emitter is a loop over the cursor into the sink, so the whole report is
     * written without the diff ever being held in memory.
     */
    if (have_format) {
        emit.format = (uint8_t)format;
        ibha_csvd_fd_sink out;
        ibha_csvd_sink sink;
        ibha_csvd_fd_sink_init(&out, 1);
        sink.write = ibha_csvd_fd_sink_write;
        sink.ctx = &out;

        ibha_csvd_status est = ibha_csvd_emit(d, &emit, &sink, NULL);
        if (est != IBHA_CSVD_OK) {
            fprintf(stderr, "ibha-csvdiff: %s: %s\n", ibha_csvd_status_name(est),
                    ibha_csvd_ctx_error(ctx));
            ibha_csvd_ctx_free(ctx);
            return EXIT_ERROR;
        }
        const ibha_csvd_diff_stats *es = ibha_csvd_diff_stats_of(d);
        int any = es->rows_modified || es->rows_added || es->rows_deleted || es->rows_moved;
        ibha_csvd_ctx_free(ctx);
        return any ? EXIT_DIFF : EXIT_SAME;
    }

    /* Draining the cursor is what fills in the cell level counts: the matcher
     * knows the row counts without reading a single cell, which is the whole
     * point of deferring the rest. */
    uint32_t report_rows = 0;
    ibha_csvd_cursor *cur = ibha_csvd_cursor_open(d);
    while (cur && ibha_csvd_cursor_next(cur) == 1) report_rows++;
    double drained = now_seconds();

    const ibha_csvd_diff_stats *s = ibha_csvd_diff_stats_of(d);
    uint64_t bytes = ibha_csvd_parse_stats_of(sp)->bytes + ibha_csvd_parse_stats_of(tp)->bytes;

    printf("source          %s\n", src_path);
    printf("target          %s\n", tgt_path);
    printf("matching        %s\n", s->all_keys ? "all columns (spec 6.4)" : "by key (spec 6.1)");
    printf("columns         %u compared", s->n_columns_compared);
    if (s->columns_added || s->columns_removed) {
        printf(", %u added and %u removed in the uploaded file", s->columns_added,
               s->columns_removed);
    }
    printf("\n");
    printf("unchanged       %u\n", s->rows_unchanged);
    printf("modified        %u  (%llu cells)\n", s->rows_modified,
           (unsigned long long)s->cells_changed);
    printf("added           %u\n", s->rows_added);
    printf("deleted         %u\n", s->rows_deleted);
    printf("moved           %u%s\n", s->rows_moved, s->moves_forced_off ? "  (forced off)" : "");
    printf("suppressed      %llu cells differ only in formatting\n",
           (unsigned long long)s->cells_suppressed);
    if (!no_validate) {
        uint64_t findings = s->cells_required_empty + s->cells_too_long + s->cells_not_numeric +
                            s->cells_bad_precision;
        printf("findings        %llu cells in %u rows (%llu required empty, %llu too long, "
               "%llu not numeric, %llu out of precision)\n",
               (unsigned long long)findings, s->rows_with_findings,
               (unsigned long long)s->cells_required_empty, (unsigned long long)s->cells_too_long,
               (unsigned long long)s->cells_not_numeric,
               (unsigned long long)s->cells_bad_precision);
    }
    if (s->paired_by_similarity) printf("paired          %u by similarity\n", s->paired_by_similarity);
    if (s->pairing_truncated) printf("warning         similarity pairing was truncated\n");
    printf("report rows     %u\n", report_rows);
    printf("arena reserved  %.2f MB\n",
           (double)ibha_csvd_ctx_bytes_reserved(ctx) / (1024.0 * 1024.0));
    printf("parse %.4f s, match %.4f s, cursor %.4f s, %.0f MB/s end to end\n", parsed - t0,
           matched - parsed, drained - matched,
           drained > t0 ? (double)bytes / (1024.0 * 1024.0) / (drained - t0) : 0.0);

    int changed = s->rows_modified || s->rows_added || s->rows_deleted || s->rows_moved;
    ibha_csvd_ctx_free(ctx);
    return changed ? EXIT_DIFF : EXIT_SAME;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(stderr);
        return EXIT_ERROR;
    }

    if (strcmp(argv[1], "--version") == 0) {
        printf("%d.%d.%d\n", IBHA_CSVD_VERSION_MAJOR, IBHA_CSVD_VERSION_MINOR,
               IBHA_CSVD_VERSION_PATCH);
        return EXIT_SAME;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(stdout);
        return EXIT_SAME;
    }
    if (strcmp(argv[1], "parse") == 0) {
        return cmd_parse(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "ingest") == 0) {
        return cmd_ingest(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "diff") == 0) {
        return cmd_diff(argc - 2, argv + 2);
    }

    fprintf(stderr, "ibha-csvdiff: unknown subcommand %s\n", argv[1]);
    usage(stderr);
    return EXIT_ERROR;
}
