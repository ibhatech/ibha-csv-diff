package com.ibhatech.csvdiff;

import java.io.Serial;

/**
 * Everything that goes wrong, carrying the engine's own name for it.
 *
 * <p>The engine holds one error per context and the first one wins, per spec 13.5,
 * so there is no error list to walk. {@link #status()} is that error's name,
 * {@code DUPLICATE_KEY}, {@code COLUMN_ORDER}, {@code NO_HEADER} and the like, taken
 * from the engine rather than restated, and the message is the engine's
 * detail, which names the offending key and both row numbers where it can. A
 * consumer switching on the failure should switch on {@code status()}: collapsing
 * the structural errors into one code would lose the only information that makes
 * the message actionable.
 *
 * <p>Validation findings are <em>not</em> errors and never arrive here. They are
 * output, per spec 13.5, and travel on {@link DiffRow#findings()}.
 */
public class CsvDiffException extends RuntimeException {

    @Serial
    private static final long serialVersionUID = 1L;

    /**
     * The engine's name for this failure.
     *
     * @serial
     */
    private final String status;

    /** The name a failure that did not come from the engine carries. */
    public static final String NOT_ENGINE = "CALLER";

    /**
     * A failure the binding raised before the engine saw the call.
     *
     * @param message what went wrong
     */
    public CsvDiffException(String message) {
        this(message, NOT_ENGINE, null);
    }

    /**
     * A failure the binding raised, wrapping the thing that caused it.
     *
     * @param message what went wrong
     * @param cause   the underlying failure
     */
    public CsvDiffException(String message, Throwable cause) {
        this(message, NOT_ENGINE, cause);
    }

    /**
     * A failure the engine reported, carrying the engine's own name for it.
     *
     * @param message what went wrong
     * @param status  the engine's status name, or {@link #NOT_ENGINE}
     * @param cause   the underlying failure, or null
     */
    public CsvDiffException(String message, String status, Throwable cause) {
        super(message, cause);
        this.status = status;
    }

    /**
     * The engine's name for this failure, or {@link #NOT_ENGINE} when the binding
     * refused the call before the engine saw it.
     *
     * @return the status name
     */
    public String status() {
        return status;
    }
}
