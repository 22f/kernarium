// SPDX-License-Identifier: Apache-2.0
package android.kernarium.sms;

import java.nio.charset.StandardCharsets;
import java.util.Base64;
import java.util.regex.Pattern;

/**
 * One message from the host: its sender and its text. The host sends it as the
 * provider call's argument, the sender and the text each in base64 of UTF-8,
 * joined by a dot, so the shell command carries no character it would read.
 */
final class Message {
    /** A number of at most 20 digits, international when it starts with +. */
    static final Pattern NUMBER = Pattern.compile("\\+?[0-9]{1,20}");
    /** A name of at most 11 letters, digits and spaces, as a network sends one. */
    static final Pattern NAME = Pattern.compile("[A-Za-z0-9 ]{1,11}");
    /** The longest text, in UTF-16 units. */
    static final int MAXIMUM_UNITS = 670;
    /**
     * The most parts a text of {@link #MAXIMUM_UNITS} takes. A part holds 67
     * units; DeliverPdu gives one back for the U+FEFF it puts before a part
     * that starts with one, and a part ends a unit early rather than split a
     * surrogate pair. So every part but the last takes at least 65 units of
     * the text, and 670 units take at most 11 parts. The host counts the
     * same (GuestSms.maximumParts).
     */
    static final int MAXIMUM_PARTS = 11;

    final String sender;
    final String body;

    private Message(String sender, String body) {
        this.sender = sender;
        this.body = body;
    }

    static Message parse(String argument) {
        if (argument == null) throw new IllegalArgumentException("no message");
        final String[] fields = argument.split("\\.", -1);
        if (fields.length != 2) throw new IllegalArgumentException("not a sender and a text");
        final String sender = decode(fields[0]);
        final String body = decode(fields[1]);
        if (!NUMBER.matcher(sender).matches() && !NAME.matcher(sender).matches()
                || sender.trim().isEmpty()) {
            throw new IllegalArgumentException("not a phone number or a sender name");
        }
        if (body.isEmpty() || body.length() > MAXIMUM_UNITS) {
            throw new IllegalArgumentException("the text must have 1 to " + MAXIMUM_UNITS + " characters");
        }
        // Android's SMS code cannot return U+FFFE (see DeliverPdu).
        if (body.indexOf('\uFFFE') >= 0) throw new IllegalArgumentException("the text has U+FFFE");
        return new Message(sender, body);
    }

    private static String decode(String field) {
        final byte[] bytes;
        try {
            bytes = Base64.getDecoder().decode(field);
        } catch (IllegalArgumentException e) {
            throw new IllegalArgumentException("not base64");
        }
        final String text = new String(bytes, StandardCharsets.UTF_8);
        // A malformed sequence decodes to U+FFFD; the host never sends one.
        if (!java.util.Arrays.equals(text.getBytes(StandardCharsets.UTF_8), bytes)) {
            throw new IllegalArgumentException("not UTF-8");
        }
        return text;
    }
}
