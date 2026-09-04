package com.ibhatech.csvdiff;

import java.io.Reader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

/**
 * What the JSON row source costs, against what a document tree of the same array
 * costs.
 *
 * <p>Driven by {@code scripts-and-commands/measure_json_rows.py}, which runs each
 * mode at ascending heap sizes and reports the smallest that completes. The claim
 * being measured is the one in {@link JsonRows}: that a row array of any size costs
 * one row of live data, and that parsing it into {@code Map} and {@code List}
 * objects instead is the heap behaviour the C engine was chosen to avoid.
 *
 * <p>In this package rather than in {@code tools} because the contrast needs
 * {@link Json}, which is package private and is meant to stay that way: it exists to
 * read a summary this library produced itself.
 */
public final class JsonBenchMain {

    private JsonBenchMain() {
    }

    static Header header() {
        return Header.builder()
                .column("id", "INTEGER").key().required()
                .column("name", "VARCHAR(40)")
                .column("region", "CHAR(2)")
                .column("premium", "DECIMAL(12,2)")
                .column("updated", "TIMESTAMP")
                .build();
    }

    public static void main(String[] args) throws Exception {
        String mode = args[0];
        Path path = Path.of(args[1]);
        long bytes = Files.size(path);

        long start = System.nanoTime();
        String what = switch (mode) {
            case "stream" -> stream(path);
            case "tree" -> tree(path);
            default -> throw new IllegalArgumentException("unknown mode " + mode);
        };
        double ms = (System.nanoTime() - start) / 1e6;

        System.out.printf("%s  %.0f ms  %.0f MB/s  %s%n",
                mode, ms, bytes / 1e6 / (ms / 1000), what);
    }

    /** The row source, both sides, which is the two CLOBs of use case 3. */
    private static String stream(Path path) throws Exception {
        Header h = header();
        try (Reader a = Files.newBufferedReader(path, StandardCharsets.UTF_8);
             Reader b = Files.newBufferedReader(path, StandardCharsets.UTF_8);
             Diff d = CsvDiff.compare(DiffSource.ofJsonRows(h, a), DiffSource.ofJsonRows(h, b))) {
            return d.summary().rows().unchanged() + " rows unchanged";
        }
    }

    /** The same array as a tree, which is what a general purpose parser hands back
     *  and what this source exists not to build. One side, not two. */
    private static String tree(Path path) throws Exception {
        Object doc = Json.parse(Files.readString(path));
        List<?> rows = (List<?>) doc;
        return rows.size() + " rows as objects";
    }
}
