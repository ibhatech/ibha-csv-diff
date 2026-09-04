package com.ibhatech.csvdiff.jni;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.Locale;

/**
 * Finds and loads the JNI library, once.
 *
 * <p>Two paths, and the first exists for development. If
 * {@code -Dibha.csvdiff.nativeLibPath} names a directory, the library is loaded
 * from there, which is how the build drives a library it has just compiled without
 * packaging it first. Otherwise it is extracted from the jar under
 * {@code native/<os>-<arch>/} to a temporary file and loaded from there, which is
 * what a consumer's {@code mvn dependency} gets.
 *
 * <p>Extraction rather than {@code System.loadLibrary}: a jar is not a directory the
 * dynamic linker can search, and requiring consumers to set
 * {@code java.library.path} would make a drop-in dependency into an ops task.
 *
 * <p>Failure is recorded, not thrown. A provider that cannot load its library is
 * unavailable, which is a thing {@link JniProvider#isAvailable()} answers and the
 * selection in {@code CsvDiff} acts on; throwing from a static initializer would
 * turn "this platform has no bundled binary" into a stack trace with
 * {@code ExceptionInInitializerError} at the top and no useful cause.
 */
final class NativeLibrary {

    private NativeLibrary() {
    }

    static final String PATH_PROPERTY = "ibha.csvdiff.nativeLibPath";

    private static final Object LOCK = new Object();
    private static boolean tried;
    private static boolean loaded;
    private static String failure;

    static boolean load() {
        synchronized (LOCK) {
            if (!tried) {
                tried = true;
                try {
                    doLoad();
                    loaded = true;
                } catch (IOException | UnsatisfiedLinkError | SecurityException e) {
                    failure = e.toString();
                }
            }
            return loaded;
        }
    }

    /** Why the load failed, for an error message that can be acted on. */
    static String failure() {
        synchronized (LOCK) {
            return failure == null ? "the native library has not been loaded" : failure;
        }
    }

    private static void doLoad() throws IOException {
        String file = libraryFileName();
        String dir = System.getProperty(PATH_PROPERTY);
        if (dir != null && !dir.isEmpty()) {
            Path direct = Path.of(dir, platform(), file);
            if (!Files.isRegularFile(direct)) direct = Path.of(dir, file);
            if (!Files.isRegularFile(direct)) {
                throw new IOException(PATH_PROPERTY + " is set to " + dir
                        + " but holds no " + file + ", with or without a " + platform()
                        + " subdirectory");
            }
            System.load(direct.toAbsolutePath().toString());
            return;
        }

        String resource = "/native/" + platform() + "/" + file;
        try (InputStream in = NativeLibrary.class.getResourceAsStream(resource)) {
            if (in == null) {
                // Name exactly what is in the jar. Claiming more sends a consumer
                // looking for a packaging fault when what they have is an
                // unsupported platform, and those cost very different amounts to
                // diagnose. Loader support is wider than the bundle on purpose:
                // any platform() this computes will load from the jar the day a
                // build for it is added, with no change here.
                throw new IOException("this jar carries no native library at " + resource
                        + ". Version " + BUNDLED_VERSION + " bundles " + BUNDLED
                        + " only. On any other platform, build the library from"
                        + " source and point " + PATH_PROPERTY + " at it:"
                        + " python3 scripts-and-commands/build_jni.py");
            }
            Path tmp = Files.createTempDirectory("ibha-csvdiff-");
            tmp.toFile().deleteOnExit();
            Path out = tmp.resolve(file);
            Files.copy(in, out, StandardCopyOption.REPLACE_EXISTING);
            out.toFile().deleteOnExit();
            System.load(out.toAbsolutePath().toString());
        }
    }

    /**
     * The platforms this artifact actually ships a native library for.
     *
     * <p>Two, because those are the two that are built and tested on real
     * hardware. The loader itself is not limited to them: {@link #platform()}
     * computes a tag for any host, and adding a build for one is a CI job plus a
     * line here rather than a code change.
     */
    private static final String BUNDLED = "linux-x86_64 and darwin-aarch64";

    private static final String BUNDLED_VERSION = "0.1.0";

    /** {@code <os>-<arch>}, matching the directory names in the jar. */
    static String platform() {
        String os = System.getProperty("os.name", "").toLowerCase(Locale.ROOT);
        String arch = System.getProperty("os.arch", "").toLowerCase(Locale.ROOT);

        String osName;
        if (os.contains("mac") || os.contains("darwin")) osName = "darwin";
        else if (os.contains("win")) osName = "windows";
        else osName = "linux";

        String archName = switch (arch) {
            case "amd64", "x86_64" -> "x86_64";
            case "aarch64", "arm64" -> "aarch64";
            default -> arch;
        };
        return osName + "-" + archName;
    }

    static String libraryFileName() {
        String os = System.getProperty("os.name", "").toLowerCase(Locale.ROOT);
        if (os.contains("mac") || os.contains("darwin")) return "libibha_csvdiff_jni.dylib";
        if (os.contains("win")) return "ibha_csvdiff_jni.dll";
        return "libibha_csvdiff_jni.so";
    }
}
