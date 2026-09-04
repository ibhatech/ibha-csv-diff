package com.ibhatech.csvdiff.jni;

import com.ibhatech.csvdiff.CsvDiffException;
import com.ibhatech.csvdiff.CsvDiffProvider;
import com.ibhatech.csvdiff.Diff;
import com.ibhatech.csvdiff.DiffOptions;
import com.ibhatech.csvdiff.DiffSource;

/**
 * The shipped backend: hand written JNI over the C engine, per spec 13.6.
 *
 * <p>Registered through {@code META-INF/services}. It is the only provider in this
 * jar and it sits at priority 0, so an FFM backend for JDK 22+ added later as a
 * separate artifact wins the selection wherever it can run, without either jar
 * knowing about the other.
 */
public final class JniProvider implements CsvDiffProvider {

    /** Required by {@link java.util.ServiceLoader}. */
    public JniProvider() {
    }

    @Override
    public String name() {
        return "jni";
    }

    @Override
    public boolean isAvailable() {
        return NativeLibrary.load();
    }

    @Override
    public int priority() {
        return 0;
    }

    @Override
    public String engineVersion() {
        require();
        int[] v = new int[3];
        NativeEngine.version(v);
        return v[0] + "." + v[1] + "." + v[2];
    }

    /** The row schema version the engine's emitters carry, which is a versioned
     * public contract: changing a row shape is a breaking change and bumps it.
     *
     * @return the row schema version
     */
    public int schemaVersion() {
        require();
        return NativeEngine.schemaVersion();
    }

    @Override
    public Diff compare(DiffSource source, DiffSource target, DiffOptions options) {
        require();
        return JniDiff.run(source, target, options);
    }

    private void require() {
        if (!NativeLibrary.load()) {
            throw new CsvDiffException("the ibha-csvdiff native library could not be loaded for "
                    + NativeLibrary.platform() + ": " + NativeLibrary.failure());
        }
    }
}
