package com.ibhatech.csvdiff.tools;

import com.ibhatech.csvdiff.CsvDiff;
import com.ibhatech.csvdiff.Diff;
import com.ibhatech.csvdiff.DiffRow;
import com.ibhatech.csvdiff.DiffSource;
import com.ibhatech.csvdiff.EmitFormat;
import com.ibhatech.csvdiff.EmitOptions;
import com.ibhatech.csvdiff.Header;
import com.ibhatech.csvdiff.RowFeedOptions;
import com.ibhatech.csvdiff.RowOptions;

import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.function.Supplier;

/**
 * What the binding costs on the p90 pair, which is the table in the README.
 *
 * <p>Run it rather than editing that table by hand:
 *
 * <pre>
 *   python3 scripts-and-commands/measure_java_binding.py
 * </pre>
 *
 * <p>Three runs are timed and the best is reported, which is enough to get past a
 * cold page cache and one round of JIT without pretending this is a microbenchmark
 * harness. The numbers that matter are ratios, not absolutes: the gap between
 * values off and values decoded is what decoding 1.7 million cells into Java
 * strings costs.
 *
 * <p>The two "batch 1" rows are the ones that argue with their own author. They run
 * the same walk with one JNI crossing per row instead of one per thousand, and on
 * this machine the difference is inside the noise. So the batch drain is cheap
 * insurance rather than the load bearing decision; the load bearing one is that
 * cell values never cross at all, and 1.76 million crossings is a different order
 * of magnitude from 147 thousand. Keeping the measurement that says so is more
 * useful than keeping the claim it deflates.
 */
public final class BenchMain {

    private BenchMain() {
    }

    private static Path source;
    private static Path target;

    public static void main(String[] args) {
        Path fixtures = Path.of(args.length > 0 ? args[0] : "core/fixtures/generated");
        source = fixtures.resolve("p90_source.csv");
        target = fixtures.resolve("p90_target.csv");

        System.out.printf("%-34s %8s   %s%n", "what", "seconds", "note");

        long[] reserved = new long[1];
        time("compare, parse and match", () -> {
            try (Diff d = open()) {
                reserved[0] = d.bytesReserved();
                return 1;
            }
        }, () -> (reserved[0] >> 20) + " MB reserved");

        try (Diff d = open()) {
            // The emitter rather than summary(), which caches: timing a cache hit
            // would report zero and mean nothing.
            time("summary, one drain", () -> d.emit(EmitFormat.SUMMARY).length,
                    () -> "constant memory");

            long[] rows = new long[1];
            time("rows(), values off", () -> {
                rows[0] = d.rows(RowOptions.defaults().withIncludeValues(false)).count();
                return 1;
            }, () -> rows[0] + " rows");

            long[] cells = new long[1];
            time("rows(), values decoded", () -> {
                cells[0] = d.rows().mapToLong(r -> r.cells().size()).sum();
                return 1;
            }, () -> cells[0] + " cells");

            long[] changed = new long[1];
            time("rows(), changes only", () -> {
                changed[0] = d.rows(RowOptions.changes()).count();
                return 1;
            }, () -> changed[0] + " rows");

            // The same walk with the batch turned off, so the cost of the JNI
            // boundary itself is a measured number rather than an assumption.
            time("rows(), values off, batch 1", () -> d.rows(RowOptions.defaults()
                    .withIncludeValues(false).withBatchRows(1)).count(),
                    () -> "one crossing per row");
            time("rows(), decoded, batch 1", () -> d.rows(RowOptions.defaults().withBatchRows(1))
                    .mapToLong(r -> r.cells().size()).sum(),
                    () -> "one crossing per row, cells decoded");

            long[] bytes = new long[1];
            time("emit jsonl, changes only", () -> {
                bytes[0] = d.emit(EmitFormat.JSONL, EmitOptions.changes()).length;
                return 1;
            }, () -> (bytes[0] >> 10) + " KB, two passes");

            time("emit jsonl, whole report", () -> {
                bytes[0] = d.emit(EmitFormat.JSONL).length;
                return 1;
            }, () -> (bytes[0] >> 20) + " MB, two passes");
        }

        benchRowFeed();
    }

    /** The JDBC path, measured against the same data so the encode pass is visible. */
    private static void benchRowFeed() {
        List<List<String>> rows = new ArrayList<>();
        try (Diff d = open()) {
            d.rows(RowOptions.defaults()).limit(50_000).forEach(r -> {
                List<String> values = new ArrayList<>(r.cells().size());
                for (var c : r.cells()) values.add(c.target().orElse(c.source().orElse("")));
                rows.add(values);
            });
        }
        Header header = Header.namesOnly(headerNames());

        for (int batch : new int[] {1000, 100}) {
            RowFeedOptions feed = RowFeedOptions.defaults().withBatchRows(batch);
            time("row feed, " + rows.size() + " rows, batch " + batch, () -> {
                Iterator<List<String>> a = rows.iterator();
                Iterator<List<String>> b = rows.iterator();
                try (Diff d = CsvDiff.compare(DiffSource.ofRows(header, a),
                        DiffSource.ofRows(header, b), com.ibhatech.csvdiff.DiffOptions.builder()
                                .headerRows(1).build())) {
                    return d.summary().rows().report();
                }
            }, () -> "encode plus parse plus match");
        }
    }

    private static List<String> headerNames() {
        try (Diff d = open()) {
            return d.columns();
        }
    }

    private static Diff open() {
        return CsvDiff.compare(source, target);
    }

    private static void time(String label, Supplier<?> work, Supplier<String> note) {
        double best = Double.MAX_VALUE;
        for (int i = 0; i < 3; i++) {
            long t0 = System.nanoTime();
            work.get();
            best = Math.min(best, (System.nanoTime() - t0) / 1e9);
        }
        System.out.printf("%-34s %8.3f   %s%n", label, best, note.get());
    }
}
