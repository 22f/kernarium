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
        int protocolIdentifier;
        int firstOctet;
        int addressType;
        String sender;
        /** Year, month, day, hour, minute, second, and the zone in quarter hours. */
        int[] time = new int[7];
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
        final Part part = new Part();
        part.firstOctet = first;
        final int addressLength = pdu[i++] & 0xff;
        part.addressType = pdu[i++] & 0xff;
        final int addressOctets = (addressLength + 1) / 2;
        part.sender = address(part.addressType, addressLength, Arrays.copyOfRange(pdu, i, i + addressOctets));
        i += addressOctets;
        part.protocolIdentifier = pdu[i++] & 0xff;
        part.dataCodingScheme = pdu[i++] & 0xff;
        for (int field = 0; field < 7; ++field) {
            final int octet = pdu[i++] & 0xff;
            final int low = octet & 0x0f;
            final int high = octet >> 4;
            part.time[field] = field < 6 ? low * 10 + high
                    : ((low & 0x07) * 10 + high) * ((low & 0x08) != 0 ? -1 : 1);
        }
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

    /** The address field as a receiver reads it: digits of a number, or a name of GSM 7-bit characters. */
    private static String address(int type, int length, byte[] octets) {
        final StringBuilder out = new StringBuilder();
        if ((type & 0x70) == 0x50) { // alphanumeric
            same(0xd0, type, "alphanumeric type of address");
            final int septets = octets.length * 8 / 7;
            for (int n = 0; n < septets && out.length() < length * 4 / 7; ++n) {
                final int bit = n * 7;
                int value = (octets[bit / 8] & 0xff) >> (bit % 8);
                if (bit % 8 > 1) value |= (octets[bit / 8 + 1] & 0xff) << (8 - bit % 8);
                out.append((char) (value & 0x7f));
            }
            return out.toString();
        }
        check(type == 0x91 || type == 0x81, "numeric type of address " + type);
        if (type == 0x91) out.append('+');
        for (int n = 0; n < length; ++n) {
            final int octet = octets[n / 2] & 0xff;
            final int digit = n % 2 == 0 ? octet & 0x0f : octet >> 4;
            out.append((char) ('0' + digit));
        }
        return out.toString();
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

    private static final String ID = "0123456789abcdef0123456789abcdef";

    private static String argument(String sender, String body) {
        return argument(sender, body, ID);
    }

    private static String argument(String sender, String body, String identifier) {
        return base64(sender) + "." + base64(body) + "." + identifier;
    }

    private static String identifier(int number) {
        return String.format("%032x", number);
    }

    /** The inbox of a stand-in for Delivery: what the service handed to Android, in order. */
    private static final class Inbox {
        final List<String> messages = new ArrayList<>();

        int deliver(Message message) {
            synchronized (messages) {
                messages.add(message.sender + ": " + message.body);
            }
            return DeliverPdu.split(message.body).size();
        }
    }

    private static DeliveredIdentifiers.Result handOver(DeliveredIdentifiers identifiers, Inbox inbox, String argument) {
        final Message message = Message.parse(argument);
        return identifiers.deliver(message.identifier, () -> inbox.deliver(message));
    }

    private static void identifierRequired() {
        final String ok = argument("Bank", "hi");
        same(ID, Message.parse(ok).identifier, "the identifier is carried");
        final String[] bad = {
            base64("Bank") + "." + base64("hi"), // none
            base64("Bank") + "." + base64("hi") + ".", // empty
            ok.substring(0, ok.length() - 1), // short
            ok + "0", // long
            argument("Bank", "hi", "0123456789ABCDEF0123456789ABCDEF"), // upper case
            argument("Bank", "hi", "0123456789abcdef0123456789abcdeg"), // not hex
            argument("Bank", "hi", "0123456789abcdef0123456789abcde-"),
            ok + ".00", // a fourth field
        };
        for (String argument : bad) {
            try {
                Message.parse(argument);
                check(false, "the parser took <" + argument + ">");
            } catch (IllegalArgumentException expected) {
                // refused
            }
        }
    }

    private static boolean accepts(String argument) {
        try {
            Message.parse(argument);
            return true;
        } catch (IllegalArgumentException e) {
            return false;
        }
    }

    /** What Message.parse accepts of the sender and the text, as it did before the identifier. */
    private static void parserRules() {
        final String[] senders = {
            "+15551234567", "12345", "1", "+1", "12345678901234567890", "+12345678901234567890", "Bank", "My Bank 24",
            "ElevenChars", "A B", "12 34", " x",
        };
        for (String sender : senders) check(accepts(argument(sender, "hi")), "accepted sender <" + sender + ">");
        final String[] notSenders = {
            "", " ", "           ", "+", "123456789012345678901", "+123456789012345678901", "TwelveChars!", "Twelve Chars",
            "B\u00E4nk", "a-b", "+12 34", "a.b", "\u0661\u0662", "Bank\n",
        };
        for (String sender : notSenders) check(!accepts(argument(sender, "hi")), "refused sender <" + sender + ">");
        final String[] texts = {
            "h", "hello", "x".repeat(670), "\uD83D\uDE00".repeat(335), "line\nbreak\r\n", "\uFEFFmark", "tab\tnul\u0000",
            "\uFFFF", "\u0645\u0631\u062D\u0628\u0627",
        };
        for (String text : texts) check(accepts(argument("Bank", text)), "accepted text " + describe(text));
        final String[] notTexts = {"", "x".repeat(671), "\uD83D\uDE00".repeat(336), "x".repeat(669) + "\uD83D\uDE00"};
        for (String text : notTexts) check(!accepts(argument("Bank", text)), "refused text " + describe(text));
        // The shape of the argument.
        check(!accepts(null), "no argument");
        check(!accepts(""), "empty argument");
        check(!accepts(base64("Bank") + base64("hi") + "." + ID), "a dot missing");
        check(!accepts("!!!." + base64("hi") + "." + ID), "sender not base64");
        check(!accepts(base64("Bank") + ".!!!." + ID), "text not base64");
        check(!accepts(base64("Bank") + "." + java.util.Base64.getEncoder().encodeToString(new byte[] {(byte) 0xc3, 0x28})
                + "." + ID), "text not UTF-8");
        check(!accepts(java.util.Base64.getEncoder().encodeToString(new byte[] {(byte) 0xc3, 0x28})
                + "." + base64("hi") + "." + ID), "sender not UTF-8");
        check(!accepts(java.util.Base64.getEncoder().encodeToString(new byte[] {(byte) 0xed, (byte) 0xa0, (byte) 0x80})
                + "." + base64("hi") + "." + ID), "an encoded surrogate is not UTF-8");
        check(!accepts(base64("Bank") + "." + java.util.Base64.getEncoder().encodeToString(
                new byte[] {(byte) 0xed, (byte) 0xa0, (byte) 0x80}) + "." + ID), "an encoded lone surrogate in the text");
        // What came through is what was sent.
        final Message message = Message.parse(argument("+15551234567", "caf\u00E9 \uD83D\uDE00"));
        same("+15551234567", message.sender, "sender");
        same("caf\u00E9 \uD83D\uDE00", message.body, "text");
    }

    /** The host sends the same identifier again when it did not see the answer: the message is stored once. */
    private static void retryAfterALostReply() {
        final DeliveredIdentifiers identifiers = new DeliveredIdentifiers();
        final Inbox inbox = new Inbox();
        final String argument = argument("+15551234567", "code 123456", identifier(1));
        final DeliveredIdentifiers.Result first = handOver(identifiers, inbox, argument);
        same(1, first.parts, "parts of the first call");
        check(!first.repeat, "the first call hands the message over");
        // The reply was lost: the same call again.
        final DeliveredIdentifiers.Result second = handOver(identifiers, inbox, argument);
        check(second.repeat, "the second call is a repeat");
        same(first.parts, second.parts, "a repeat answers with the first call's parts");
        same(List.of("+15551234567: code 123456"), inbox.messages, "stored once");
        // The same text under a new identifier is a new message.
        handOver(identifiers, inbox, argument("+15551234567", "code 123456", identifier(2)));
        same(2, inbox.messages.size(), "the same text again, as a new message");
        // A call that fails is not remembered: its retry hands the message over.
        final String failing = argument("Bank", "later", identifier(3));
        try {
            identifiers.deliver(Message.parse(failing).identifier, () -> {
                throw new IllegalStateException("broadcast refused");
            });
            check(false, "the failure was swallowed");
        } catch (IllegalStateException expected) {
            // passed on to the caller
        }
        check(!handOver(identifiers, inbox, failing).repeat, "a failed call is tried again");
        same(3, inbox.messages.size(), "stored after the retry of a failure");
    }

    /** Messages in flight at once never share a call, and one identifier is never handed over twice. */
    private static void messagesInFlight() throws InterruptedException {
        final DeliveredIdentifiers identifiers = new DeliveredIdentifiers();
        final Inbox inbox = new Inbox();
        final java.util.concurrent.CountDownLatch inside = new java.util.concurrent.CountDownLatch(1);
        final java.util.concurrent.CountDownLatch release = new java.util.concurrent.CountDownLatch(1);
        final java.util.concurrent.atomic.AtomicInteger deliveries = new java.util.concurrent.atomic.AtomicInteger();
        final List<DeliveredIdentifiers.Result> results = java.util.Collections.synchronizedList(new ArrayList<>());
        final Message a = Message.parse(argument("Alice", "first " + "a".repeat(100), identifier(10)));
        final Message b = Message.parse(argument("Bob", "second", identifier(11)));
        // A is handed over slowly; B and a retry of A arrive meanwhile.
        final Thread slowA = new Thread(() -> results.add(identifiers.deliver(a.identifier, () -> {
            deliveries.incrementAndGet();
            inside.countDown();
            try {
                release.await();
            } catch (InterruptedException e) {
                throw new IllegalStateException(e);
            }
            return inbox.deliver(a);
        })));
        slowA.start();
        inside.await();
        final Thread otherB = new Thread(() -> results.add(identifiers.deliver(b.identifier, () -> {
            deliveries.incrementAndGet();
            return inbox.deliver(b);
        })));
        final Thread retryA = new Thread(() -> results.add(identifiers.deliver(a.identifier, () -> {
            deliveries.incrementAndGet();
            return inbox.deliver(a);
        })));
        otherB.start();
        retryA.start();
        release.countDown();
        slowA.join(20_000);
        otherB.join(20_000);
        retryA.join(20_000);
        check(!slowA.isAlive() && !otherB.isAlive() && !retryA.isAlive(), "the calls finish");
        same(2, deliveries.get(), "A and B are each handed over once");
        same(3, results.size(), "three answers");
        same(2, inbox.messages.size(), "two messages stored");
        check(inbox.messages.contains("Alice: " + a.body) && inbox.messages.contains("Bob: second"),
                "each message is stored as it was sent: " + inbox.messages);
        int repeats = 0;
        for (DeliveredIdentifiers.Result result : results) {
            if (result.repeat) ++repeats;
        }
        same(1, repeats, "the retry of A is the one repeat");
        for (DeliveredIdentifiers.Result result : results) {
            check(result.parts == 1 || result.parts == DeliverPdu.split(a.body).size(), "parts " + result.parts);
        }
    }

    /** The memory is bounded: the oldest identifier goes first. */
    private static void identifierMemory() {
        final DeliveredIdentifiers identifiers = new DeliveredIdentifiers();
        final Inbox inbox = new Inbox();
        for (int i = 0; i < DeliveredIdentifiers.CAPACITY; ++i) {
            handOver(identifiers, inbox, argument("Bank", "m" + i, identifier(1000 + i)));
        }
        check(handOver(identifiers, inbox, argument("Bank", "m0", identifier(1000))).repeat,
                "the oldest is still known at the capacity");
        handOver(identifiers, inbox, argument("Bank", "new", identifier(5000)));
        check(!handOver(identifiers, inbox, argument("Bank", "m0", identifier(1000))).repeat,
                "past the capacity the oldest is forgotten");
        check(handOver(identifiers, inbox, argument("Bank", "new", identifier(5000))).repeat, "the newest is known");
    }

    /** The most parts: the bound in Message, reached by a text of surrogate pairs on every split. */
    private static String hex(byte[] bytes) {
        final StringBuilder out = new StringBuilder();
        for (byte b : bytes) out.append(String.format("%02X", b & 0xff));
        return out.toString();
    }

    /** One PDU byte for byte, worked out from the specification by hand. */
    private static void goldenPdu() {
        // 2023-11-14 22:13:20 UTC. No service centre, SMS-DELIVER; 11 digits from an international number;
        // protocol 0, UCS-2; the time stamp; 4 octets of user data.
        final byte[][] single = DeliverPdu.encode("+15551234567", "Hi", NOW, ZONE, 0x5a);
        same(1, single.length, "one part");
        same("00040B91515521436" + "5F7" + "0008" + "32114122310200" + "04" + "00480069", hex(single[0]), "numeric sender, short text");
        // Alphanumeric: 4 characters pack into 4 octets, 7 useful semi-octets, type 0xD0.
        final byte[][] named = DeliverPdu.encode("Bank", "Hi", NOW, ZONE, 0x5a);
        same("0004" + "07D0C2B07B0D" + "0008" + "32114122310200" + "04" + "00480069", hex(named[0]), "alphanumeric sender");
        // A number without + is a national number (type 0x81), and an odd count of digits ends with a filler.
        final byte[][] national = DeliverPdu.encode("12345", "Hi", NOW, ZONE, 0x5a);
        same("0004" + "05" + "81" + "2143F5" + "0008" + "32114122310200" + "04" + "00480069", hex(national[0]), "national number");
        // Concatenated: the header 05 00 03 reference total sequence counts in the length, and sets bit 6.
        final byte[][] two = DeliverPdu.encode("12345", "a".repeat(71), NOW, ZONE, 0x1ff);
        final String head = "0044" + "05" + "81" + "2143F5" + "0008" + "32114122310200";
        check(hex(two[0]).startsWith(head + "8C" + "050003FF0201" + "0061"), "the first part of two: " + hex(two[0]));
        same(16 + 1 + 6 + 134, two[0].length, "the first part is 67 units and the header");
        same(head + "0E" + "050003FF0202" + "0061006100610061", hex(two[1]), "the second part, 4 units");
        same(2, two.length, "71 units take two parts");
    }

    private static void fields() {
        final String[] senders = {"+15551234567", "15551234567", "1", "+12", "12345678901234567890", "+98912123456",
            "Bank", "My Bank 24", "A", "ElevenChars", "0"};
        for (String sender : senders) {
            for (String text : new String[] {"Hi", "a".repeat(100)}) {
                final byte[][] pdus = DeliverPdu.encode(sender, text, NOW, ZONE, 7);
                for (byte[] pdu : pdus) {
                    final Part part = read(pdu);
                    same(sender, part.sender, "sender <" + sender + ">");
                    same(0, part.protocolIdentifier, "protocol identifier");
                    same(pdus.length == 1 ? 0x04 : 0x44, part.firstOctet, "first octet");
                    if (pdus.length > 1) same(7, part.reference, "reference");
                }
            }
        }
        // The thresholds of one part, two and three, and of a text that is not the first alphabet's.
        same(1, encode("a".repeat(70)).length, "70 units");
        same(2, encode("a".repeat(71)).length, "71 units");
        same(2, encode("a".repeat(134)).length, "134 units");
        same(3, encode("a".repeat(135)).length, "135 units");
        same(140, read(encode("a".repeat(70))[0]).userData.length, "a full single part");
        same(134, read(encode("a".repeat(71))[0]).userData.length, "a full concatenated part");
        // The reference is one octet.
        same(0xff, read(DeliverPdu.encode("12345", "a".repeat(100), NOW, ZONE, -1)[0]).reference, "reference masked");
        // A surrogate pair is never split between parts.
        for (int p = 55; p < 75; ++p) {
            final String body = filled(140, new int[] {p, p + 1}, HIGH, LOW);
            for (String piece : DeliverPdu.split(body)) {
                check(!Character.isHighSurrogate(piece.charAt(piece.length() - 1)), "a part ends in a high surrogate, pair at " + p);
                check(!Character.isLowSurrogate(piece.charAt(0)), "a part starts with a low surrogate, pair at " + p);
            }
            roundTrip(body);
        }
        // The time stamp is the local time of the zone, with the zone in quarter hours.
        final TimeZone india = TimeZone.getTimeZone("GMT+05:30");
        final TimeZone pacific = TimeZone.getTimeZone("GMT-08:00");
        same("[23, 11, 14, 22, 13, 20, 0]", Arrays.toString(read(DeliverPdu.encode("12345", "Hi", NOW, ZONE, 1)[0]).time), "UTC");
        same("[23, 11, 15, 3, 43, 20, 22]", Arrays.toString(read(DeliverPdu.encode("12345", "Hi", NOW, india, 1)[0]).time), "+05:30");
        same("[23, 11, 14, 14, 13, 20, -32]", Arrays.toString(read(DeliverPdu.encode("12345", "Hi", NOW, pacific, 1)[0]).time), "-08:00");
    }

    private static void partCount() {
        same(11, Message.MAXIMUM_PARTS, "the stated maximum");
        // Every part but the last takes at least 65 units of the text (a part that starts with a mark and
        // ends before a surrogate pair), so 670 units cannot take a twelfth part, and 11 parts need more
        // than the 10 * 65 units that the first ten parts take.
        check((Message.MAXIMUM_PARTS - 1) * 65 + 1 <= Message.MAXIMUM_UNITS, "the maximum is reachable");
        check(Message.MAXIMUM_PARTS * 65 + 1 > Message.MAXIMUM_UNITS, "the maximum cannot be exceeded");
        // 66 units, then ten pairs that each begin a part and end the one before it a unit early.
        final String pair = "" + HIGH + LOW;
        final StringBuilder worst = new StringBuilder("a".repeat(66));
        for (int part = 2; part <= 10; ++part) worst.append(pair).append("a".repeat(64));
        worst.append(pair);
        same(662, worst.length(), "the worst case");
        same(11, DeliverPdu.split(worst.toString()).size(), "parts of the worst case");
        roundTrip(worst.toString());
        // The same with a mark first: its part takes 65 units of the text and 66 on the wire.
        final StringBuilder marked = new StringBuilder(MARK + "a".repeat(64));
        for (int part = 2; part <= 10; ++part) marked.append(pair).append("a".repeat(64));
        marked.append(pair);
        same(11, DeliverPdu.split(marked.toString()).size(), "parts of the worst case with a mark");
        roundTrip(marked.toString());
        // A mark at the start of every part: 66 units of the text each.
        final String chain = (MARK + "a".repeat(65)).repeat(10) + "b";
        same(11, DeliverPdu.split(chain).size(), "parts of marks");
        roundTrip(chain);
        // Nothing of the longest length takes more.
        final String[] stuffing = {"a", "" + HIGH + LOW, "" + MARK};
        for (String filler : stuffing) {
            for (int offset = 0; offset < 4; ++offset) {
                final StringBuilder text = new StringBuilder("c".repeat(offset));
                while (text.length() < Message.MAXIMUM_UNITS) text.append(filler);
                final String body = text.substring(0, Message.MAXIMUM_UNITS)
                        .replaceAll("[\uD800-\uDBFF]$", "");
                check(DeliverPdu.split(body).size() <= Message.MAXIMUM_PARTS, "parts of " + describe(body));
                roundTrip(body);
            }
        }
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

    public static void main(String[] args) throws Exception {
        decoderContract();
        byteOrderMark();
        reversedMark();
        goldenPdu();
        fields();
        partCount();
        random();
        parserRules();
        identifierRequired();
        retryAfterALostReply();
        messagesInFlight();
        identifierMemory();
        if (failures != 0) {
            System.out.println(failures + " failure(s)");
            System.exit(1);
        }
        System.out.println("PASS");
    }
}
