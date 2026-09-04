package com.ibhatech.csvdiff.tools;

import com.ibhatech.csvdiff.CellDiffMode;
import com.ibhatech.csvdiff.CsvDiff;
import com.ibhatech.csvdiff.CsvDiffException;
import com.ibhatech.csvdiff.Diff;
import com.ibhatech.csvdiff.DiffOptions;
import com.ibhatech.csvdiff.DiffSource;
import com.ibhatech.csvdiff.EmitFormat;
import com.ibhatech.csvdiff.EmitOptions;

import java.io.IOException;
import java.io.OutputStream;
import java.nio.file.Path;

/**
 * Drives one emitter and writes its bytes to stdout, taking the same flags the C
 * CLI takes.
 *
 * <p>It exists for {@code check_determinism.mjs}, which compares the native build,
 * the two wasm builds and this binding byte for byte over a fixture and emitter
 * matrix. Spec section 9 asks that the golden suite assert that every build
 * produces byte identical output, and a Java binding that quietly rendered a
 * decimal differently, or wrote a class name in a different order, would otherwise
 * be discovered by a customer comparing a server report against a browser preview.
 *
 * <p>It is a test source rather than part of the artifact: the library's job is to
 * be embedded, and shipping a second CLI that shadows the real one would be a
 * maintenance surface with no consumer.
 *
 * <pre>
 *   java -cp target/classes:target/test-classes com.ibhatech.csvdiff.tools.EmitMain \
 *        source.csv target.csv --header 4 --format jsonl --changes-only
 * </pre>
 *
 * <p>Exit codes follow {@code diff(1)}, as the CLI does: 0 identical, 1 differences
 * found, 2 error.
 */
public final class EmitMain {

    private EmitMain() {
    }

    public static void main(String[] args) {
        try {
            System.exit(run(args));
        } catch (CsvDiffException e) {
            System.err.println("ibha-csvdiff-java: " + e.status() + ": " + e.getMessage());
            System.exit(2);
        } catch (IOException e) {
            System.err.println("ibha-csvdiff-java: " + e);
            System.exit(2);
        }
    }

    private static int run(String[] args) throws IOException {
        String source = null;
        String target = null;
        int headerRows = 4;
        EmitFormat format = EmitFormat.JSONL;
        boolean changesOnly = false;
        boolean validate = true;
        boolean detectMoves = true;
        int maxRows = 0;
        CellDiffMode cellDiff = CellDiffMode.NONE;

        for (int i = 0; i < args.length; i++) {
            switch (args[i]) {
                case "--header" -> headerRows = Integer.parseInt(args[++i]);
                case "--format" -> format = switch (args[++i]) {
                    case "jsonl" -> EmitFormat.JSONL;
                    case "csv" -> EmitFormat.CSV;
                    case "html" -> EmitFormat.HTML;
                    case "summary" -> EmitFormat.SUMMARY;
                    default -> throw new CsvDiffException("unknown format " + args[i]);
                };
                case "--changes-only" -> changesOnly = true;
                case "--no-validate" -> validate = false;
                case "--no-moves" -> detectMoves = false;
                case "--max-rows" -> maxRows = Integer.parseInt(args[++i]);
                case "--cell-diff" -> cellDiff = switch (args[++i]) {
                    case "word" -> CellDiffMode.WORD;
                    case "char" -> CellDiffMode.CHARACTER;
                    case "both" -> CellDiffMode.WORD_THEN_CHARACTER;
                    default -> throw new CsvDiffException("unknown cell diff mode " + args[i]);
                };
                default -> {
                    if (args[i].startsWith("-")) throw new CsvDiffException("unknown option " + args[i]);
                    if (source == null) source = args[i];
                    else if (target == null) target = args[i];
                    else throw new CsvDiffException("unexpected argument " + args[i]);
                }
            }
        }
        if (source == null || target == null) {
            System.err.println("usage: EmitMain <source.csv> <target.csv> [--header N]"
                    + " [--format jsonl|csv|html|summary] [--changes-only] [--no-validate]"
                    + " [--no-moves] [--max-rows N] [--cell-diff word|char|both]");
            return 2;
        }

        DiffOptions options = DiffOptions.builder()
                .headerRows(headerRows)
                .validate(validate)
                .detectMoves(detectMoves)
                .build();

        EmitOptions emit = EmitOptions.defaults()
                .withChangesOnly(changesOnly)
                .withCellDiff(cellDiff)
                .withMaxRows(maxRows);

        try (Diff d = CsvDiff.compare(DiffSource.ofFile(Path.of(source)),
                DiffSource.ofFile(Path.of(target)), options);
             OutputStream out = System.out) {
            d.emitTo(out, format, emit);
            out.flush();
            return d.summary().identical() ? 0 : 1;
        }
    }
}
