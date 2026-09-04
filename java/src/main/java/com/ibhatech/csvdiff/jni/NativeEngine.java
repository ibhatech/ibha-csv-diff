package com.ibhatech.csvdiff.jni;

import java.nio.ByteBuffer;

/**
 * The C ABI, one Java method per entry point, and nothing else.
 *
 * <p>Every method here is {@code static native} and takes primitives, arrays and
 * direct {@link ByteBuffer}s. Handles cross as {@code long}. No object is
 * constructed on the C side and no C field is named on the Java side, which is
 * where a hand written JNI binding usually starts to rot.
 *
 * <p><strong>These signatures are checked against the C, not trusted.</strong> The
 * build runs {@code javac -h} over this class to generate the header the glue
 * includes, so a parameter added, removed or retyped here is a compile error in C
 * rather than an {@code UnsatisfiedLinkError} in production. That is the whole
 * reason the header is generated at build time instead of being written by hand.
 *
 * <p>Anything that could carry non-ASCII crosses as {@code byte[]} holding real
 * UTF-8. JNI's own string conversion produces <em>modified</em> UTF-8, which is not
 * the same encoding, and file data is exactly where the difference shows up.
 */
final class NativeEngine {

    private NativeEngine() {
    }

    /* ----------------------------------------------------------- row records -- */

    /** Bytes per row in a cursor batch: a fixed head plus one flag byte per column,
     *  padded. Asked for rather than computed on both sides. */
    static native int rowRecordStride(int nColumns);

    /** Offsets within one row record, in ints. The head is eight ints and the flag
     *  bytes follow it. */
    static final int REC_KIND = 0;
    static final int REC_MOVED = 1;
    static final int REC_MOVE_DISTANCE = 2;
    static final int REC_SOURCE_ROW = 3;
    static final int REC_TARGET_ROW = 4;
    static final int REC_CHANGED = 5;
    static final int REC_SUPPRESSED = 6;
    static final int REC_FINDINGS = 7;
    static final int REC_HEAD_BYTES = 32;

    /* --------------------------------------------------------------- version -- */

    static native void version(int[] out3);

    static native String statusName(int status);

    static native int schemaVersion();

    /* --------------------------------------------------------------- context -- */

    static native long ctxNew(long maxBytes, int maxRows, int maxColumns);

    static native void ctxFree(long ctx);

    static native int ctxStatus(long ctx);

    /** UTF-8 bytes, not a String. See the class comment. */
    static native byte[] ctxError(long ctx);

    static native long ctxBytesReserved(long ctx);

    /* ------------------------------------------------------- comparison opts -- */

    static native long compareOptsNew(int trimWhitespace, int charIgnorePad, int numeric,
                                      int booleans, int dateCompare, byte[] boolTrue,
                                      byte[] boolFalse, int allowAdded, int allowRemoved);

    static native void compareOptsFree(long compare);

    /* ----------------------------------------------------------------- parse -- */

    static native long parseBegin(long ctx, long compare, int delimiter, int quote, int stripBom,
                                  int headerRows, int keyRow, int requiredRow, int typeRow,
                                  int nameRow, long expectTable, long expectSchema, long sizeHint);

    static native int parseChunk(long parser, ByteBuffer direct, int len);

    static native int parseFinish(long parser);

    static native long tableOf(long parser);

    static native long schemaOf(long parser);

    /* -------------------------------------------------------- columnar views -- */

    static native ByteBuffer tableBytes(long table);

    static native ByteBuffer tableFieldOff(long table);

    static native ByteBuffer tableFieldLen(long table);

    static native ByteBuffer tableFieldFlags(long table);

    static native ByteBuffer tableRowFirstField(long table);

    /** nRows, nColumns, nFields, byte length, quote byte. */
    static native void tableDims(long table, int[] out5);

    static native int fieldCopy(long table, int field, ByteBuffer dst, int cap);

    static native int fieldLogicalLen(long table, int field);

    /* ---------------------------------------------------------------- schema -- */

    /** nColumns, nKeyColumns, keyRow, requiredRow, typeRow, nameRow, firstDataRow,
     *  namesOnly. Absent rows are -1. */
    static native void schemaInts(long schema, int[] out8);

    static native void schemaColumns(long schema, int n, byte[] flags, byte[] types, int[] sizes,
                                     int[] scales);

    /* ------------------------------------------------------------------ diff -- */

    static native long diffRun(long ctx, long compare, long srcTable, long srcSchema, long tgtTable,
                               long tgtSchema, int detectMoves, int sourceOrdered,
                               int countSuppressed, int validate, int requireKey,
                               int deletedPlacement, int similarityK, int similarityPercent);

    static native long diffTable(long diff, int side);

    static native long diffSchema(long diff, int side);

    static native int diffColumns(long diff);

    /** The row level counters only. The cell level ones accumulate as a cursor
     *  advances, so they come from the summary emitter instead. */
    static native void diffRowStats(long diff, int[] out12);

    /* ---------------------------------------------------------------- cursor -- */

    static native long cursorOpen(long diff);

    static native int cursorNextBatch(long cursor, ByteBuffer out, int maxRows, int nColumns);

    /* -------------------------------------------------------------- segments -- */

    static native int cellSegments(long diff, int sourceRow, int targetRow, int col, int mode,
                                   int maxBytes, ByteBuffer out, int cap);

    /* ------------------------------------------------------------------ emit -- */

    static native long emit(long diff, int format, int changesOnly, int includeValues, int cellDiff,
                            int maxCellBytes, int maxRows, int csvFormulaGuard, int csvDelimiter,
                            byte[] classPrefix, ByteBuffer out, int cap);

    /* ------------------------------------------------------- engine constants -- */

    /** {@code IBHA_CSVD_FIELD_HAS_ESCAPE}: the field contains a doubled quote, so
     *  its byte range is not its logical value and the engine has to collapse it. */
    static final int FIELD_HAS_ESCAPE = 0x02;

    /** {@code IBHA_CSVD_COL_KEY}. */
    static final int COL_KEY = 0x01;

    static final int CELL_CHANGED = 0x01;
    static final int CELL_SUPPRESSED = 0x02;
    static final int CELL_FINDING = 0x3c;

    /** {@code IBHA_CSVD_NO_ROW}, as it arrives after the C side converts it. */
    static final int NO_ROW = -1;
}
