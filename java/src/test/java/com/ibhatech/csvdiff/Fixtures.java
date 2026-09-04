package com.ibhatech.csvdiff;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;

/** The generated corpus the C tests and the JS tests also run against. */
final class Fixtures {

    private Fixtures() {
    }

    static Path dir() {
        String dir = System.getProperty("ibha.csvdiff.fixtures");
        if (dir == null || dir.isEmpty()) {
            throw new IllegalStateException(
                    "set -Dibha.csvdiff.fixtures to core/fixtures/generated");
        }
        Path p = Path.of(dir);
        if (!Files.isDirectory(p)) {
            throw new IllegalStateException(p + " is not a directory. Run "
                    + "python3 core/fixtures/gen_fixtures.py");
        }
        return p;
    }

    static Path path(String name) {
        return dir().resolve(name);
    }

    static byte[] bytes(String name) {
        try {
            return Files.readAllBytes(path(name));
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
    }

    /** The 15 MB pair, which is the p90 preview case the throughput targets are
     *  written against. */
    static boolean hasP90() {
        return Files.isRegularFile(path("p90_source.csv"));
    }
}
