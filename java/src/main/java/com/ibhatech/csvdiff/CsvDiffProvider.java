package com.ibhatech.csvdiff;

/**
 * A backend that can run comparisons.
 *
 * <p>This exists because of a version boundary rather than because anyone wanted a
 * plugin system. Spec 13.6 pins this binding to JDK 21, where the Foreign Function
 * and Memory API is a preview feature under JEP 442 and was not final until JEP 454
 * in JDK 22. A library cannot demand {@code --enable-preview} of its consumers, so
 * the shipped backend is hand written JNI. Putting the API behind an interface with
 * the implementation chosen at runtime means an FFM backend can be added later, as
 * a separate artifact on a newer JDK, and picked up automatically by applications
 * that can use it, without a single visible change to consumers of {@link Diff}.
 *
 * <p>Providers are discovered with {@link java.util.ServiceLoader}. The available
 * one with the highest {@link #priority()} wins, and
 * {@code -Dibha.csvdiff.provider=<name>} overrides the choice entirely, which is
 * what a test comparing two backends against each other needs.
 */
public interface CsvDiffProvider {

    /**
     * Short stable name, used by the {@code ibha.csvdiff.provider} override.
     *
     * @return the provider's name
     */
    String name();

    /**
     * Whether this provider can run here.
     *
     * <p>For the JNI backend this is whether the native library for this platform
     * could be loaded. It must not throw: a provider that cannot run is not an
     * error, it is a provider that loses the selection.
     *
     * @return true when this provider can run on this machine
     */
    boolean isAvailable();

    /**
     * Higher wins. The JNI backend is 0; an FFM backend would sit above it.
     *
     * @return this provider's selection priority
     */
    default int priority() {
        return 0;
    }

    /**
     * The version of the engine behind this provider.
     *
     * @return the engine version, as {@code major.minor.patch}
     */
    String engineVersion();

    /**
     * Runs one comparison. The source side is schema-authoritative, per spec 13.8.
     *
     * @param source the schema-authoritative side, per spec 13.8
     * @param target the side compared against it
     * @param options the comparison settings
     * @return the diff, which the caller closes
     */
    Diff compare(DiffSource source, DiffSource target, DiffOptions options);
}
