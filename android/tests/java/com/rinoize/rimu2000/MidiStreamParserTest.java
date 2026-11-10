package com.rinoize.rimu2000;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Run with javac/java; no Android runtime or ROMs required. */
public final class MidiStreamParserTest {
    private static final class Capture implements MidiStreamParser.Sink {
        final List<byte[]> messages = new ArrayList<>();
        public void send(byte[] data, int offset, int count) {
            messages.add(Arrays.copyOfRange(data, offset, offset + count));
        }
        void expect(int index, int... values) {
            byte[] expected = bytes(values);
            check(index < messages.size() && Arrays.equals(messages.get(index), expected),
                    "Message " + index + ": expected " + Arrays.toString(expected));
        }
    }

    public static void main(String[] args) {
        runningStatusAndConcurrentControls();
        sysexAndRealtime();
        commonMessagesAndReset();
        sysexBoundaryAndOverflow();
        malformedRecovery();
        arbitraryChunkBoundaries();
        System.out.println("MIDI parser: 6 scenarios passed");
    }

    private static void runningStatusAndConcurrentControls() {
        Capture out = new Capture(); MidiStreamParser parser = new MidiStreamParser(out);
        feed(parser, 0x92, 60);
        check(out.messages.isEmpty(), "Partial note must not escape");
        // Model a UI controller event reaching the shared queue between chunks.
        out.send(bytes(0xb0, 7, 100), 0, 3);
        feed(parser, 100, 61, 101, 0x82);
        feed(parser, 60, 0);
        check(out.messages.size() == 4, "Running status message count");
        out.expect(0, 0xb0, 7, 100);
        out.expect(1, 0x92, 60, 100);
        out.expect(2, 0x92, 61, 101);
        out.expect(3, 0x82, 60, 0);
    }

    private static void sysexAndRealtime() {
        Capture out = new Capture(); MidiStreamParser parser = new MidiStreamParser(out);
        feed(parser, 0xf0, 0x43, 0x10);
        feed(parser, 0xf8, 0x4c, 0, 0, 0x7e);
        check(out.messages.size() == 1, "Only realtime may escape partial SysEx");
        out.send(bytes(0xb0, 64, 0), 0, 3);
        feed(parser, 0, 0xf7);
        check(out.messages.size() == 3, "SysEx must be one atomic message");
        out.expect(0, 0xf8);
        out.expect(1, 0xb0, 64, 0);
        out.expect(2, 0xf0, 0x43, 0x10, 0x4c, 0, 0, 0x7e, 0, 0xf7);
    }

    private static void commonMessagesAndReset() {
        Capture out = new Capture(); MidiStreamParser parser = new MidiStreamParser(out);
        feed(parser, 0xc1, 5, 6, 0xd1, 30, 0xf8, 31, 0xf2, 1);
        feed(parser, 2, 60, 100, 0xf1, 7, 0xf3, 8, 0xf6, 0xf5, 4);
        check(out.messages.size() == 10, "System common message count");
        out.expect(0, 0xc1, 5); out.expect(1, 0xc1, 6);
        out.expect(2, 0xd1, 30); out.expect(3, 0xf8); out.expect(4, 0xd1, 31);
        out.expect(5, 0xf2, 1, 2); out.expect(6, 0xf1, 7);
        out.expect(7, 0xf3, 8); out.expect(8, 0xf6); out.expect(9, 0xf5, 4);
        check(parser.strayBytes() == 2, "System common must cancel running status");
    }

    private static void sysexBoundaryAndOverflow() {
        int max = MidiStreamParser.MAX_SYSEX_BYTES;
        Capture out = new Capture(); MidiStreamParser parser = new MidiStreamParser(out);
        byte[] exact = new byte[max]; exact[0] = (byte)0xf0; exact[max - 1] = (byte)0xf7;
        parser.accept(exact, 0, max - 1);
        check(out.messages.isEmpty(), "Max-size message still waits for EOX");
        parser.accept(exact, max - 1, 1);
        check(out.messages.size() == 1 && out.messages.get(0).length == max,
                "Exactly 64 KiB SysEx must fit");
        check(parser.oversizedSysex() == 0, "Exact boundary is not overflow");
        out.messages.clear();
        byte[] oversized = new byte[max]; oversized[0] = (byte)0xf0;
        parser.accept(oversized, 0, oversized.length);
        feed(parser, 0x90, 60, 100, 0xf8, 0xf0, 1, 0xf7, 0x90, 62, 100);
        check(parser.oversizedSysex() == 1, "One oversized message must be counted once");
        check(out.messages.size() == 2, "Overflow tail must not turn into notes");
        out.expect(0, 0xf8); out.expect(1, 0x90, 62, 100);
    }

    private static void malformedRecovery() {
        Capture out = new Capture(); MidiStreamParser parser = new MidiStreamParser(out);
        feed(parser, 0xf0, 0x43, 0x90, 60, 100, 61, 0x80, 60, 0);
        check(out.messages.size() == 2, "Interrupted partial messages must not escape");
        out.expect(0, 0x90, 60, 100); out.expect(1, 0x80, 60, 0);
        check(parser.interruptedMessages() == 2, "Interrupted SysEx and note must be counted");
    }

    private static void arbitraryChunkBoundaries() {
        byte[] stream = bytes(0x90, 60, 100, 61, 101, 0xf8, 62, 102,
                0xf0, 0x43, 0x10, 0x4c, 0, 0, 0x7e, 0, 0xf7,
                0x80, 60, 0, 61, 0, 62, 0, 0xc0, 10, 11);
        Capture reference = new Capture();
        new MidiStreamParser(reference).accept(stream, 0, stream.length);
        for (int chunk = 1; chunk <= stream.length; chunk++) {
            Capture out = new Capture(); MidiStreamParser parser = new MidiStreamParser(out);
            for (int offset = 0; offset < stream.length; offset += chunk) {
                parser.accept(stream, offset, Math.min(chunk, stream.length - offset));
            }
            check(out.messages.size() == reference.messages.size(), "Chunk-independent count " + chunk);
            for (int i = 0; i < out.messages.size(); i++) {
                check(Arrays.equals(out.messages.get(i), reference.messages.get(i)),
                        "Chunk-independent framing " + chunk + "/" + i);
            }
        }
    }

    private static void feed(MidiStreamParser parser, int... values) {
        byte[] data = bytes(values); parser.accept(data, 0, data.length);
    }
    private static byte[] bytes(int... values) {
        byte[] data = new byte[values.length];
        for (int i = 0; i < data.length; i++) data[i] = (byte)values[i];
        return data;
    }
    private static void check(boolean ok, String why) {
        if (!ok) throw new AssertionError(why);
    }
}
