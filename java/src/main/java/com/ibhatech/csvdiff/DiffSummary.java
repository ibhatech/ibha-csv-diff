package com.ibhatech.csvdiff;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;

/**
 * The counts, the findings and the schema findings, in constant memory whatever the
 * size of the diff.
 *
 * <p><strong>This is parsed from the summary emitter's own bytes rather than read
 * out of the engine's stats struct</strong>, and the reason is not tidiness. The
 * cell level counters accumulate as a cursor advances, so reading the struct gives
 * a number that depends on how many times the caller happened to have drained the
 * report; the summary emitter zeroes them and drains its own cursor, so its numbers
 * are always those of exactly one pass. Parsing its JSON also means the summary
 * this binding reports and the summary written to a file cannot drift.
 *
 * <p>{@link #json()} is those bytes, so a caller that wants a field this record does
 * not model, or that wants to forward the summary verbatim to a queue or an email,
 * has it without a re-encode.
 *
 * @param schemaVersion      the summary schema's own version, so a consumer that
 *                           stored a report can tell which shape it is reading
 * @param identical          true when the two sides agree on every row and cell,
 *                           which is the answer most callers want without reading
 *                           any count
 * @param rows               row counts by change kind
 * @param cells              cell counts, changed and suppressed
 * @param findings           validation findings, per spec 13.5
 * @param matching           how the two sides were paired, per spec 6
 * @param columns            column counts, compared, added and removed
 * @param targetHeader       what header auto-detection made of the target file
 * @param schemaFindings     the schema disagreements, capped for the report
 * @param schemaFindingCount how many there were in total, which can exceed the
 *                           size of {@code schemaFindings} when the cap was hit
 * @param json               the emitter's own bytes, decoded, so a field this
 *                           record does not model is still reachable
 */
public record DiffSummary(
        int schemaVersion,
        boolean identical,
        Rows rows,
        Cells cells,
        Findings findings,
        Matching matching,
        Columns columns,
        TargetHeader targetHeader,
        List<SchemaFinding> schemaFindings,
        long schemaFindingCount,
        String json) {

    /**
     * Defensively copies the schema findings, so a summary handed to a consumer
     * cannot be edited under it.
     */
    public DiffSummary {
        schemaFindings = List.copyOf(schemaFindings);
    }

    /**
     * Row counts. Final the moment the match completes: the matcher knows them
     * without looking at a single cell.
     *
     * @param unchanged rows present on both sides with every cell equal
     * @param modified  rows present on both sides with at least one cell changed
     * @param added     rows in the target with no match in the source
     * @param deleted   rows in the source with no match in the target
     * @param moved     rows outside the longest increasing subsequence, per spec 6.2
     * @param report    rows the report will carry, which is fewer than the total
     *                  when changes-only is on
     */
    public record Rows(long unchanged, long modified, long added, long deleted, long moved,
                       long report) {
    }

    /**
     * Cell counts. These are the work the cursor exists to defer, which is why
     * they come from a drain rather than from a struct.
     *
     * @param changed    cells whose values differ once normalized
     * @param suppressed cells that differ in bytes but are equal once normalized,
     *                   which is what makes a normalization rule auditable rather
     *                   than invisible
     */
    public record Cells(long changed, long suppressed) {
    }

    /**
     * Validation findings, per spec 13.5.
     *
     * @param total         every finding on every row
     * @param rows          how many rows carry at least one finding
     * @param requiredEmpty cells empty in a column the header marks REQUIRED
     * @param tooLong       cells over their declared VARCHAR or CHAR length,
     *                      counted in characters, per spec 13.13.5
     * @param notNumeric    cells that do not parse as their declared numeric type
     * @param precision     cells outside their declared precision or scale
     * @param enabled       false when the caller turned validation off, which is
     *                      what distinguishes "no findings" from "not looked for"
     */
    public record Findings(long total, long rows, long requiredEmpty, long tooLong, long notNumeric,
                           long precision, boolean enabled) {
    }

    /**
     * How the two sides were matched, per spec 6.
     *
     * @param allKeys            every row was matched by its declared key, which is
     *                           the case a reconciliation wants
     * @param pairedBySimilarity rows paired by content rather than by key
     * @param pairingTruncated   the similarity search hit its candidate limit, so
     *                           some pairing that was possible was not attempted
     * @param movesForcedOff     move detection was skipped because the input was
     *                           too large for it
     */
    public record Matching(boolean allKeys, long pairedBySimilarity, boolean pairingTruncated,
                           boolean movesForcedOff) {
    }

    /**
     * Column counts. {@code added} and {@code removed} are non zero only when the
     * caller allowed a column difference; the default policy makes one an error.
     *
     * @param compared columns present on both sides and actually compared
     * @param added    columns in the target and not in the source
     * @param removed  columns in the source and not in the target
     */
    public record Columns(long compared, long added, long removed) {
    }

    /**
     * What header auto-detection made of the target file, per spec 13.8.
     *
     * @param rows      how many header rows were detected in the target
     * @param namesOnly the uploaded file carried only a column name row and
     *                  inherited the rest from the source
     */
    public record TargetHeader(long rows, boolean namesOnly) {
    }

    /**
     * A disagreement about schema, which is reported rather than fatal.
     *
     * <p>{@code kind} is {@code columnAdded}, {@code columnRemoved} or
     * {@code metadataDisagreement}. The last is the target file's header rows
     * disagreeing with the source's, where the source wins, per spec 13.8: the
     * uploaded file's opinion about which column is a key is not something we act
     * on, but it is something worth telling the caller about.
     *
     * @param kind   {@code columnAdded}, {@code columnRemoved} or
     *               {@code metadataDisagreement}
     * @param column the 0 based column this concerns
     * @param name   that column's name
     * @param row    which header row disagreed, for a metadata disagreement
     * @param source what the source file's header said, where the two differ
     * @param target what the target file's header said, where the two differ
     */
    public record SchemaFinding(String kind, int column, String name, Optional<String> row,
                                Optional<String> source, Optional<String> target) {
    }

    /**
     * Parses the summary emitter's output.
     *
     * <p>Public because it is what a backend outside this package calls, and
     * because it is independently useful: a summary written to a file by this
     * library or by the CLI reads back through the same code that produced the one
     * on a live handle.
     *
     * @param utf8 the summary emitter's output
     * @return the parsed summary
     */
    public static DiffSummary parse(byte[] utf8) {
        String text = new String(utf8, StandardCharsets.UTF_8);
        Object root = Json.parse(text);

        Object r = Json.obj(root, "rows");
        Object c = Json.obj(root, "cells");
        Object f = Json.obj(root, "findings");
        Object m = Json.obj(root, "matching");
        Object col = Json.obj(root, "columns");
        Object th = Json.obj(root, "targetHeader");

        List<SchemaFinding> sf = new ArrayList<>();
        for (Object o : Json.arr(root, "schemaFindings")) {
            if (!(o instanceof Map<?, ?>)) continue;
            sf.add(new SchemaFinding(
                    Json.str(o, "kind"),
                    (int) Json.num(o, "column"),
                    Json.str(o, "name"),
                    Json.has(o, "row") ? Optional.of(Json.str(o, "row")) : Optional.empty(),
                    Json.has(o, "source") ? Optional.of(Json.str(o, "source")) : Optional.empty(),
                    Json.has(o, "target") ? Optional.of(Json.str(o, "target")) : Optional.empty()));
        }

        return new DiffSummary(
                (int) Json.num(root, "schemaVersion"),
                Json.bool(root, "identical"),
                new Rows(Json.num(r, "unchanged"), Json.num(r, "modified"), Json.num(r, "added"),
                        Json.num(r, "deleted"), Json.num(r, "moved"), Json.num(r, "report")),
                new Cells(Json.num(c, "changed"), Json.num(c, "suppressed")),
                new Findings(Json.num(f, "total"), Json.num(f, "rows"), Json.num(f, "requiredEmpty"),
                        Json.num(f, "tooLong"), Json.num(f, "notNumeric"), Json.num(f, "precision"),
                        Json.bool(f, "enabled")),
                new Matching(Json.bool(m, "allKeys"), Json.num(m, "pairedBySimilarity"),
                        Json.bool(m, "pairingTruncated"), Json.bool(m, "movesForcedOff")),
                new Columns(Json.num(col, "compared"), Json.num(col, "added"),
                        Json.num(col, "removed")),
                new TargetHeader(Json.num(th, "rows"), Json.bool(th, "namesOnly")),
                sf,
                Json.num(root, "schemaFindingCount"),
                text);
    }
}
