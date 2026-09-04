package com.ibhatech.csvdiff;

import org.junit.jupiter.api.Test;

import java.util.ServiceLoader;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * The backend is selected at runtime, per spec 13.6, so that an FFM implementation
 * for JDK 22+ can be added later without a change visible to consumers.
 *
 * <p>These assert the mechanism rather than the choice: that a provider is
 * discovered through {@link ServiceLoader}, that it reports itself available, and
 * that the public API goes through it. When a second backend exists, the same
 * mechanism is what a differential test between the two would drive.
 */
class ProviderTest {

    @Test
    void aBackendIsDiscoveredThroughServiceLoader() {
        boolean found = false;
        for (CsvDiffProvider p : ServiceLoader.load(CsvDiffProvider.class)) {
            found = true;
            assertNotNull(p.name());
        }
        assertTrue(found, "the jar must register at least one backend in META-INF/services");
    }

    @Test
    void theShippedBackendIsTheJniOneAndItCanRunHere() {
        CsvDiffProvider p = CsvDiff.provider();
        assertEquals("jni", p.name());
        assertTrue(p.isAvailable());
        assertEquals(0, p.priority(), "an FFM backend would sit above this");
    }

    @Test
    void theEngineVersionComesFromTheEngineNotFromTheJar() {
        assertEquals(CsvDiff.provider().engineVersion(), CsvDiff.engineVersion());
        assertTrue(CsvDiff.engineVersion().matches("\\d+\\.\\d+\\.\\d+"));
    }

    /** The row schema is a versioned public contract shared with the emitters, so a
     *  consumer can move between a streamed row and a saved report. */
    @Test
    void theRowSchemaVersionIsTakenFromTheEngine() {
        com.ibhatech.csvdiff.jni.JniProvider jni =
                (com.ibhatech.csvdiff.jni.JniProvider) CsvDiff.provider();
        assertEquals(1, jni.schemaVersion());

        try (Diff d = CsvDiff.compare(Fixtures.path("tiny_source.csv"),
                Fixtures.path("tiny_target.csv"))) {
            assertEquals(jni.schemaVersion(), d.summary().schemaVersion());
        }
    }
}
