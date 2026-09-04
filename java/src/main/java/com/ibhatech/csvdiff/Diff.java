package com.ibhatech.csvdiff;

import java.io.OutputStream;
import java.util.List;
import java.util.stream.Stream;

/**
 * One comparison, held open over engine memory.
 *
 * <p><strong>Nothing here materializes the diff.</strong> {@link #rows()} walks the
 * engine's cursor and decodes exactly the row it is standing on. A 90,000 row diff
 * as Java objects is a few million allocations, and avoiding that is most of why
 * the engine is where it is. Peak memory during a walk is the two indexes plus one
 * batch of rows, never the size of the report.
 *
 * <p><strong>Close it.</strong> The context and its whole arena are native memory,
 * and the arena frees nothing individually because it releases everything at once.
 * A handle that is dropped without closing keeps its bytes until the cleaner gets
 * to it, which on a batch worker running hundreds of comparisons is the difference
 * between flat memory and a climb.
 *
 * <p>This is an interface, and the implementation is selected at runtime, so an FFM
 * backend for JDK 22+ can be added later without any change visible here. See
 * {@link CsvDiffProvider}.
 *
 * {@snippet :
 * try (Diff d = CsvDiff.compare(DiffSource.ofFile(source), DiffSource.ofFile(target))) {
 *     if (!d.summary().identical()) {
 *         d.rows(RowOptions.changesOnly()).forEach(row -> report(row));
 *     }
 * }
 * }
 */
public interface Diff extends AutoCloseable {

    /**
     * The compared columns, in the source's order.
     *
     * <p>Under the column policy of spec 6.6 this is the columns the two files have
     * in common, which is not always the source file's column list. It is always
     * the same length as a row's cell list.
     *
     * @return the compared column names, in the source's order
     */
    List<String> columns();

    /**
     * Indices into {@link #columns()} of the key columns, in order. Empty when the
     * schema declares no key, which is the all-keys path of spec 6.4.
     *
     * @return the key column indices
     */
    List<Integer> keyColumns();

    /**
     * Counts, findings and schema findings, in constant memory. Computed once and
     * cached: it costs one drain of the report.
     *
     * @return the summary
     */
    DiffSummary summary();

    /**
     * The report, one row at a time, with values. The stream is lazy and ordered;
     * closing it early is fine and stops the walk.
     *
     * @return the report rows
     */
    Stream<DiffRow> rows();

    /**
     * The report, one row at a time.
     *
     * @param options what to include, and how much of it
     * @return the report rows
     */
    Stream<DiffRow> rows(RowOptions options);

    /**
     * Runs one of the engine's four emitters and returns its bytes.
     *
     * @param format which emitter to run
     * @return the whole report
     */
    byte[] emit(EmitFormat format);

    /**
     * Runs one of the engine's four emitters and returns its bytes.
     *
     * @param format  which emitter to run
     * @param options the emitter settings
     * @return the whole report
     */
    byte[] emit(EmitFormat format, EmitOptions options);

    /**
     * Runs an emitter and writes it to a stream, returning the byte count.
     *
     * <p>This does not stream from the engine, and saying so is more useful than
     * implying otherwise: {@code ibha_csvd_emit} drains the whole diff in one call,
     * so the report is produced in memory and then written. What it does save is a
     * second copy on the Java side. A resumable emitter is the fix and it belongs in
     * the engine.
     *
     * @param out     where to write, which this does not close
     * @param format  which emitter to run
     * @param options the emitter settings
     * @return how many bytes were written
     */
    long emitTo(OutputStream out, EmitFormat format, EmitOptions options);

    /**
     * Intra cell segments for one column of one report row, per spec 7.
     *
     * <p>The row may have been walked past already: the engine reads only the two
     * row indices off it, which is what lets a reviewer click a cell long after the
     * cursor moved on. Returns an empty list when the values are equal, when either
     * side is absent, or when the mode is {@link CellDiffMode#NONE}.
     *
     * <p>Segment offsets are into the UTF-8 bytes of the value, not into a Java
     * string. See {@link TextSegment}.
     *
     * @param row    the report row the cell belongs to
     * @param column the 0 based index into {@link #columns()}
     * @param mode   how finely to segment
     * @return the segments, or empty when there is nothing to show
     */
    List<TextSegment> cellSegments(DiffRow row, int column, CellDiffMode mode);

    /**
     * Bytes the engine has reserved from the system, so a batch driver can size its
     * concurrency, per spec 2.6.5.
     *
     * @return the reserved byte count
     */
    long bytesReserved();

    /** Frees the context and every allocation made from its arena. */
    @Override
    void close();
}
