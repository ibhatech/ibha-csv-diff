package com.ibhatech.csvdiff;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * Derives a target from a source by editing it, the same way
 * {@code check_determinism.mjs} does.
 *
 * <p>Several fixtures ship as a single file, and comparing a file against itself
 * exercises the unchanged path and nothing else, which is not where a decode bug
 * would hide. One changed cell, one deleted row and one appended row is enough to
 * make every report row kind appear.
 *
 * <p>It splits on <em>record</em> boundaries rather than on newlines, because a
 * newline inside a quoted field is not a record boundary and treating it as one
 * produces a target with an unterminated quote. Multiline fields are exactly the
 * sort of input these tests exist to cover.
 *
 * <p>Latin-1 throughout: some fixtures are deliberately not valid UTF-8, and
 * round tripping them through a UTF-8 decode would repair the very thing being
 * tested.
 */
final class Stir {

    private Stir() {
    }

    static byte[] edit(byte[] source, int headerRows) {
        List<String> records = splitRecords(new String(source, StandardCharsets.ISO_8859_1));

        List<String> head = new ArrayList<>(records.subList(0, Math.min(headerRows, records.size())));
        List<String> body = new ArrayList<>();
        for (int i = headerRows; i < records.size(); i++) {
            if (!records.get(i).isEmpty()) body.add(records.get(i));
        }
        if (body.isEmpty()) return source;

        body.set(0, body.get(0).replace(",ACTIVE,", ",CANCELLED,").replace(",1.00,", ",1.50,"));
        if (body.size() > 1) body.remove(1);
        String last = body.get(body.size() - 1);
        int comma = last.indexOf(',');
        String firstField = comma < 0 ? last : last.substring(0, comma);
        body.add(firstField + "Z" + (comma < 0 ? "" : last.substring(comma)));

        List<String> out = new ArrayList<>(head);
        out.addAll(body);
        out.add("");
        return String.join("\n", out).getBytes(StandardCharsets.ISO_8859_1);
    }

    private static List<String> splitRecords(String text) {
        List<String> out = new ArrayList<>();
        int start = 0;
        boolean inQuotes = false;
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if (c == '"') {
                // A doubled quote inside a quoted field is an escaped quote, not
                // the end of one.
                if (inQuotes && i + 1 < text.length() && text.charAt(i + 1) == '"') i++;
                else inQuotes = !inQuotes;
            } else if (c == '\n' && !inQuotes) {
                out.add(text.substring(start, i));
                start = i + 1;
            }
        }
        out.add(text.substring(start));
        return out;
    }
}
