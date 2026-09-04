package com.ibhatech.csvdiff;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.util.List;

/**
 * A {@link ResultSet} over a list of rows, built with a dynamic proxy.
 *
 * <p>No JDBC driver is involved on purpose. What is under test is the binding's own
 * feed path, the header rows it writes and the values it renders, and pulling in
 * H2 or SQLite to exercise it would mean testing the binding through someone else's
 * type mapping, which is precisely the variance {@link SqlValues} exists to remove.
 * The tests that need a real driver are integration tests and belong in a
 * deployment, not here.
 *
 * <p>A proxy rather than a class implementing {@code ResultSet}: the interface has
 * around two hundred methods and a stub of it would be four hundred lines of
 * {@code throw}, in which the six that matter would be invisible.
 */
final class FakeResultSet {

    private FakeResultSet() {
    }

    /** The columns are whatever the row objects hold; nulls mean SQL NULL. */
    static ResultSet of(int columnCount, List<List<Object>> rows) {
        Handler handler = new Handler(columnCount, rows);
        return (ResultSet) Proxy.newProxyInstance(FakeResultSet.class.getClassLoader(),
                new Class<?>[] {ResultSet.class}, handler);
    }

    private static final class Handler implements InvocationHandler {
        private final int columnCount;
        private final List<List<Object>> rows;
        private int at = -1;
        private boolean wasNull;

        Handler(int columnCount, List<List<Object>> rows) {
            this.columnCount = columnCount;
            this.rows = rows;
        }

        @Override
        public Object invoke(Object proxy, Method method, Object[] args) {
            return switch (method.getName()) {
                case "next" -> ++at < rows.size();
                case "getObject" -> {
                    Object v = rows.get(at).get((Integer) args[0] - 1);
                    wasNull = v == null;
                    yield v;
                }
                case "wasNull" -> wasNull;
                case "getMetaData" -> metaData();
                case "close" -> null;
                case "toString" -> "FakeResultSet(" + rows.size() + " rows)";
                case "hashCode" -> System.identityHashCode(proxy);
                case "equals" -> proxy == args[0];
                default -> throw new UnsupportedOperationException(
                        "FakeResultSet does not implement " + method.getName()
                                + ". If the binding now calls it, that is a change worth noticing");
            };
        }

        private ResultSetMetaData metaData() {
            return (ResultSetMetaData) Proxy.newProxyInstance(
                    FakeResultSet.class.getClassLoader(), new Class<?>[] {ResultSetMetaData.class},
                    (p, m, a) -> switch (m.getName()) {
                        case "getColumnCount" -> columnCount;
                        case "toString" -> "FakeResultSetMetaData";
                        case "hashCode" -> System.identityHashCode(p);
                        case "equals" -> p == a[0];
                        default -> throw new UnsupportedOperationException(
                                "the binding must not read " + m.getName() + " from the driver."
                                        + " The header is supplied by the caller, deliberately");
                    });
        }
    }
}
