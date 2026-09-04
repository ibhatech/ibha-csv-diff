package com.ibhatech.csvdiff;

import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.ServiceLoader;

/**
 * The entry point.
 *
 * {@snippet :
 * try (Diff d = CsvDiff.compare(Path.of("source.csv"), Path.of("uploaded.csv"))) {
 *     DiffSummary s = d.summary();
 *     System.out.println(s.rows().modified() + " rows changed");
 *     d.rows(RowOptions.changesOnly()).forEach(row -> {
 *         for (DiffCell c : row.cells()) {
 *             if (c.changed()) {
 *                 System.out.println(c.name() + ": " + c.source().orElse("") + " -> "
 *                                    + c.target().orElse(""));
 *             }
 *         }
 *     });
 * }
 * }
 *
 * <p>One implementation of diff semantics, per spec section 9. The parser, the
 * matcher, the comparators and the emitters are in the C core; this package feeds
 * it bytes and decodes the rows it hands back, and decides nothing about the diff.
 * That is what makes a server side reconciliation and a browser preview of the same
 * pair the same report rather than two opinions.
 */
public final class CsvDiff {

    private CsvDiff() {
    }

    /** {@code -Dibha.csvdiff.provider=jni} pins the backend. */
    public static final String PROVIDER_PROPERTY = "ibha.csvdiff.provider";

    private static volatile CsvDiffProvider selected;

    /**
     * Compares two files with the default options.
     *
     * @param source the schema-authoritative side, per spec 13.8
     * @param target the side compared against it
     * @return the diff, which the caller closes
     */
    public static Diff compare(Path source, Path target) {
        return compare(DiffSource.ofFile(source), DiffSource.ofFile(target), DiffOptions.defaults());
    }

    /**
     * Compares two files.
     *
     * @param source the schema-authoritative side, per spec 13.8
     * @param target the side compared against it
     * @param options the comparison settings
     * @return the diff, which the caller closes
     */
    public static Diff compare(Path source, Path target, DiffOptions options) {
        return compare(DiffSource.ofFile(source), DiffSource.ofFile(target), options);
    }

    /**
     * Compares two sources with the default options.
     *
     * @param source the schema-authoritative side, per spec 13.8
     * @param target the side compared against it
     * @return the diff, which the caller closes
     */
    public static Diff compare(DiffSource source, DiffSource target) {
        return compare(source, target, DiffOptions.defaults());
    }

    /**
     * Compares two sources.
     *
     * <p>The <strong>source</strong> side is schema-authoritative, per spec 13.8:
     * which columns are keys, which are required and what types they carry are read
     * from it, and the target's header row count is detected against it. Getting the
     * two the wrong way round does not fail, it silently reconciles against the
     * uploaded file's opinion of the schema.
     *
     * @param source the schema-authoritative side, per spec 13.8
     * @param target the side compared against it
     * @param options the comparison settings
     * @return the diff, which the caller closes
     */
    public static Diff compare(DiffSource source, DiffSource target, DiffOptions options) {
        return provider().compare(source, target, options);
    }

    /**
     * The version of the engine behind the selected provider.
     *
     * @return the engine version, as {@code major.minor.patch}. A bug report needs
     *         this and the binding's own version, which are not the same number
     */
    public static String engineVersion() {
        return provider().engineVersion();
    }

    /**
     * The selected backend.
     *
     * <p>Discovered with {@link ServiceLoader}: the available provider with the
     * highest priority wins, and {@link #PROVIDER_PROPERTY} overrides the choice.
     *
     * @return the provider every {@code compare} on this class runs through
     */
    public static CsvDiffProvider provider() {
        CsvDiffProvider p = selected;
        if (p == null) {
            synchronized (CsvDiff.class) {
                p = selected;
                if (p == null) {
                    p = select();
                    selected = p;
                }
            }
        }
        return p;
    }

    /**
     * Pins the backend, for a test that wants to drive one in particular.
     *
     * @param provider the provider to use from now on, in place of the discovered one
     */
    public static void setProvider(CsvDiffProvider provider) {
        selected = provider;
    }

    private static CsvDiffProvider select() {
        List<CsvDiffProvider> all = new ArrayList<>();
        for (CsvDiffProvider p : ServiceLoader.load(CsvDiffProvider.class,
                CsvDiff.class.getClassLoader())) {
            all.add(p);
        }
        if (all.isEmpty()) {
            throw new CsvDiffException(
                    "no ibha-csvdiff backend is registered. The jar should carry one; a shaded or"
                            + " repackaged build that dropped META-INF/services is the usual cause");
        }

        String want = System.getProperty(PROVIDER_PROPERTY);
        if (want != null && !want.isEmpty()) {
            for (CsvDiffProvider p : all) {
                if (!p.name().equals(want)) continue;
                if (!p.isAvailable()) {
                    throw new CsvDiffException("the backend \"" + want + "\" was requested by "
                            + PROVIDER_PROPERTY + " but is not available here");
                }
                return p;
            }
            throw new CsvDiffException("no such ibha-csvdiff backend: \"" + want + "\". Registered: "
                    + all.stream().map(CsvDiffProvider::name).toList());
        }

        return all.stream()
                .filter(CsvDiffProvider::isAvailable)
                .max(Comparator.comparingInt(CsvDiffProvider::priority))
                .orElseThrow(() -> new CsvDiffException(
                        "no ibha-csvdiff backend can run here. Registered but unavailable: "
                                + all.stream().map(CsvDiffProvider::name).toList()
                                + ". For the JNI backend this normally means no native library was"
                                + " bundled for this platform"));
    }
}
