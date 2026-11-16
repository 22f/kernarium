// SPDX-License-Identifier: GPL-2.0-or-later
//
// The text message service (android/lineage-24/overlay/sms) on a plain JVM: the PDUs it builds for a
// message, read back with a parser and a decoder that the encoder does not share, the way Android reads
// them. The package is the service's, so that the test reaches what the service keeps to itself.
// test_sms_service.py compiles and runs it. The platform's own decoder is the JDK's "UTF-16" charset:
// SmsMessage decodes the user data of a UCS-2 message with new String(bytes, "utf-16"), which reads a
// leading byte order mark and drops it, so an independent UTF-16BE decoder alone cannot catch what it does.

package android.kernarium.sms;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.List;
import java.util.Random;
import java.util.TimeZone;

public final class SmsServiceTest {
    private static int failures;

    private static void check(boolean condition, String what) {
        if (!condition) {
            failures++;
            if (failures <= 40) System.out.println("FAIL: " + what);
        }
    }

    private static void same(Object expected, Object actual, String what) {
        check(expected.equals(actual), what + ": expected <" + expected + "> but got <" + actual + ">");
    }

    private static final char MARK = '\uFEFF';
    private static final char REVERSED = '\uFFFE';
    private static final char HIGH = '\uD83D';
    private static final char LOW = '\uDE00';
    private static final TimeZone ZONE = TimeZone.getTimeZone("UTC");
    private static final long NOW = 1_700_000_000_000L;

    /** One PDU as the receiving side reads it, with nothing shared with DeliverPdu. */
    private static final class Part {
        int reference = -1;
        int total = 1;
        int sequence = 1;
        int dataCodingScheme;
        byte[] userData;

        /** What SmsMessage does with the user data of a UCS-2 message. */
        String text() {
            return new String(userData, StandardCharsets.UTF_16);
        }
    }

    private static Part read(byte[] pdu) {
        int i = 0;
        i += (pdu[i] & 0xff) + 1; // service centre address
        final int first = pdu[i++] & 0xff;
        final boolean header = (first & 0x40) != 0;
        check((first & 0x03) == 0, "SMS-DELIVER");
        final int addressDigits = pdu[i++] & 0xff;
        i++; // type of address
        i += (addressDigits + 1) / 2;
        i++; // protocol identifier
        final Part part = new Part();
        part.dataCodingScheme = pdu[i++] & 0xff;
        i += 7; // time stamp
        final int length = pdu[i++] & 0xff;
        int dataLength = length;
        if (header) {
            same(5, pdu[i] & 0xff, "header length");
            same(0, pdu[i + 1] & 0xff, "concatenation, 8-bit reference");
            same(3, pdu[i + 2] & 0xff, "its length");
            part.reference = pdu[i + 3] & 0xff;
            part.total = pdu[i + 4] & 0xff;
            part.sequence = pdu[i + 5] & 0xff;
            i += 6;
            dataLength -= 6;
        }
        check(dataLength >= 0 && i + dataLength == pdu.length, "the user data length is the rest of the PDU");
        part.userData = Arrays.copyOfRange(pdu, i, pdu.length);
        return part;
    }

    /** The message as the receiver puts it together: the parts in order, each decoded as the platform does. */
    private static String received(byte[][] pdus) {
        final Part[] parts = new Part[pdus.length];
        for (byte[] pdu : pdus) {
            final Part part = read(pdu);
            same(pdus.length, part.total, "total parts");
            check(part.sequence >= 1 && part.sequence <= parts.length && parts[part.sequence - 1] == null,
                    "part " + part.sequence + " is new");
            parts[part.sequence - 1] = part;
        }
        final StringBuilder text = new StringBuilder();
        for (Part part : parts) text.append(part.text());
        return text.toString();
    }

    private static byte[][] encode(String body) {
        return DeliverPdu.encode("+15551234567", body, NOW, ZONE, 0x5a);
    }

    private static String describe(String body) {
        final StringBuilder out = new StringBuilder();
        for (int i = 0; i < body.length() && i < 12; ++i) out.append(String.format("%04X ", (int) body.charAt(i)));
        return "[" + body.length() + " units: " + out + (body.length() > 12 ? "...]" : "]");
    }

    /** Encodes, reads back like the platform and checks the text and the sizes. */
    private static void roundTrip(String body) {
        final byte[][] pdus;
        try {
            pdus = encode(body);
        } catch (RuntimeException e) {
            check(false, "encode " + describe(body) + " threw " + e);
            return;
        }
        same(body, received(pdus), "round trip " + describe(body));
        for (byte[] pdu : pdus) {
            final Part part = read(pdu);
            same(0x08, part.dataCodingScheme, "UCS-2");
            check(part.userData.length % 2 == 0, "whole units");
            check(part.userData.length <= (pdus.length == 1 ? 140 : 134),
                    "part size " + part.userData.length + " of " + describe(body));
        }
    }

    private static String filled(int length, int[] positions, char... characters) {
        final char[] units = new char[length];
        Arrays.fill(units, 'a');
        for (int i = 0; i < positions.length; ++i) units[positions[i]] = characters[i];
        return new String(units);
    }

    /** The defect itself, on the platform's decoder: what a plain UTF-16BE encoding would lose or swap. */
    private static void decoderContract() {
        same("abc", new String("\uFEFFabc".getBytes(StandardCharsets.UTF_16BE), StandardCharsets.UTF_16),
                "a leading U+FEFF is consumed as the byte order mark");
        same("\uFEFFabc", new String("\uFEFF\uFEFFabc".getBytes(StandardCharsets.UTF_16BE), StandardCharsets.UTF_16),
                "a second one survives");
        same("\u6100\u6200", new String("\uFFFEab".getBytes(StandardCharsets.UTF_16BE), StandardCharsets.UTF_16),
                "a leading U+FFFE swaps every byte after it");
        same("a\uFFFDb", new String("a\uFFFEb".getBytes(StandardCharsets.UTF_16BE), StandardCharsets.UTF_16),
                "U+FFFE elsewhere is malformed");
        same("a\uFEFFb", new String("a\uFEFFb".getBytes(StandardCharsets.UTF_16BE), StandardCharsets.UTF_16),
                "U+FEFF elsewhere is kept");
    }

    private static void byteOrderMark() {
        // The ends of the single-part range, with the mark first.
        for (int length = 1; length <= 80; ++length) {
            roundTrip(MARK + "b".repeat(length - 1));
            roundTrip("b".repeat(length));
        }
        roundTrip("" + MARK + MARK);
        roundTrip("" + MARK + MARK + MARK);
        roundTrip(MARK + "" + HIGH + LOW);
        // One mark at every position of a text that spans three parts, so that one lands on the start of
        // each part, whatever the marks before it did to the split.
        for (int p = 0; p < 150; ++p) roundTrip(filled(150, new int[] {p}, MARK));
        // Two marks at every pair of positions.
        for (int p = 0; p < 150; ++p) {
            for (int q = p + 1; q < 150; ++q) roundTrip(filled(150, new int[] {p, q}, MARK, MARK));
        }
        // A surrogate pair at every position, a mark at every other: pairs that straddle the split, marks
        // right after them and right before them.
        for (int p = 0; p < 143; ++p) {
            for (int q = 0; q < 145; ++q) {
                if (q == p || q == p + 1) continue;
                roundTrip(filled(145, new int[] {p, p + 1, q}, HIGH, LOW, MARK));
            }
        }
        // Every part starting with a mark, to the end of the longest text.
        final StringBuilder marks = new StringBuilder();
        while (marks.length() < Message.MAXIMUM_UNITS) marks.append(MARK).append("b".repeat(65));
        roundTrip(marks.substring(0, Message.MAXIMUM_UNITS));
        roundTrip(String.valueOf(MARK).repeat(Message.MAXIMUM_UNITS));
        // A mark inside a part is not doubled.
        same(List.of("ab" + MARK + "c"), DeliverPdu.split("ab" + MARK + "c"), "a mark inside a part");
        same(List.of("" + MARK + MARK + "abc"), DeliverPdu.split(MARK + "abc"), "a mark first is doubled");
    }

    private static void reversedMark() {
        for (int p = 0; p < 150; ++p) {
            final String body = filled(150, new int[] {p}, REVERSED);
            try {
                encode(body);
                check(false, "the encoder took U+FFFE at " + p);
            } catch (IllegalArgumentException expected) {
                // refused
            }
            try {
                Message.parse(argument("Bank", body));
                check(false, "the parser took U+FFFE at " + p);
            } catch (IllegalArgumentException expected) {
                // refused
            }
        }
    }

    private static String base64(String text) {
        return Base64.getEncoder().encodeToString(text.getBytes(StandardCharsets.UTF_8));
    }

    private static String argument(String sender, String body) {
        return base64(sender) + "." + base64(body);
    }

    private static void random() {
        final Random random = new Random(0x5eed);
        final String[] pieces = {"a", "b", "\u00E9", "\u3042", "\u0000", "\n", "" + MARK, "" + MARK, "" + HIGH + LOW, "\uD83D\uDC4D"};
        for (int run = 0; run < 3000; ++run) {
            final StringBuilder text = new StringBuilder();
            final int length = 1 + random.nextInt(Message.MAXIMUM_UNITS);
            while (text.length() < length) text.append(pieces[random.nextInt(pieces.length)]);
            roundTrip(text.substring(0, text.length() > Message.MAXIMUM_UNITS ? Message.MAXIMUM_UNITS : text.length())
                    .replaceAll("[\uD800-\uDBFF]$", ""));
        }
    }

    public static void main(String[] args) {
        decoderContract();
        byteOrderMark();
        reversedMark();
        random();
        if (failures != 0) {
            System.out.println(failures + " failure(s)");
            System.exit(1);
        }
        System.out.println("PASS");
    }
}
