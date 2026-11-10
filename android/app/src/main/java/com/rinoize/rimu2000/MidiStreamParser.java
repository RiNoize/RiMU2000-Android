package com.rinoize.rimu2000;

/**
 * Reconstructs one MIDI byte stream before other producers can share the native
 * queue. The sink must copy synchronously; the scratch arrays are reused.
 * No Android types, so framing/overflow behavior can be tested on a desktop JVM.
 */
final class MidiStreamParser {
    static final int MAX_SYSEX_BYTES = 65536;
    interface Sink { void send(byte[] data, int offset, int count); }

    private final Sink sink;
    private final byte[] shortMessage = new byte[3];
    private final byte[] realtime = new byte[1];
    private final byte[] sysex = new byte[MAX_SYSEX_BYTES];
    private int runningStatus = -1;
    private int shortUsed, shortExpected, sysexUsed;
    private boolean inSysex, discardingSysex;
    private volatile long oversizedSysex;
    private volatile long interruptedMessages;
    private volatile long strayBytes;

    MidiStreamParser(Sink sink) { this.sink = sink; }

    long oversizedSysex() { return oversizedSysex; }
    long interruptedMessages() { return interruptedMessages; }
    long strayBytes() { return strayBytes; }

    void accept(byte[] bytes, int offset, int count) {
        if (bytes == null || offset < 0 || count < 0 || offset > bytes.length - count) {
            throw new IllegalArgumentException("Invalid MIDI byte range");
        }
        for (int end = offset + count; offset < end; offset++) accept(bytes[offset] & 255);
    }

    private void accept(int value) {
        // Realtime is independent of framing and never cancels running status.
        if (value >= 0xf8) {
            realtime[0] = (byte)value;
            sink.send(realtime, 0, 1);
            return;
        }
        if (inSysex) {
            if (discardingSysex) {
                // Once oversized, consume everything except realtime until EOX.
                // In particular, do not reinterpret its tail as notes/controllers.
                if (value == 0xf7) endSysex();
                return;
            }
            if (value == 0xf7) {
                sysex[sysexUsed++] = (byte)value;
                sink.send(sysex, 0, sysexUsed);
                endSysex();
                return;
            }
            if (value < 0x80) {
                // Reserve the last byte for EOX: exactly 65536 bytes is valid.
                if (sysexUsed == MAX_SYSEX_BYTES - 1) {
                    oversizedSysex++;
                    discardingSysex = true;
                    sysexUsed = 0;
                } else sysex[sysexUsed++] = (byte)value;
                return;
            }
            // A fresh non-realtime status can recover a malformed short SysEx.
            // The unfinished message has never reached the native engine.
            interruptedMessages++;
            endSysex();
        }
        if (value >= 0x80) {
            if (shortUsed != 0) interruptedMessages++;
            shortUsed = 0;
            shortExpected = 0;
            if (value < 0xf0) {
                runningStatus = value;
                beginShort(value, channelLength(value));
                return;
            }
            runningStatus = -1; // System common, including SysEx, cancels it.
            if (value == 0xf0) {
                inSysex = true;
                sysex[0] = (byte)value;
                sysexUsed = 1;
            } else if (value == 0xf1 || value == 0xf3 || value == 0xf5) {
                // F5 nn is S-MU2000's documented cable/port selection extension.
                beginShort(value, 2);
            } else if (value == 0xf2) {
                beginShort(value, 3);
            } else if (value == 0xf6 || value == 0xf7) {
                shortMessage[0] = (byte)value;
                sink.send(shortMessage, 0, 1);
            } else {
                strayBytes++; // Undefined F4.
            }
            return;
        }
        if (shortUsed == 0) {
            if (runningStatus < 0) { strayBytes++; return; }
            beginShort(runningStatus, channelLength(runningStatus));
        }
        shortMessage[shortUsed++] = (byte)value;
        if (shortUsed == shortExpected) {
            sink.send(shortMessage, 0, shortUsed);
            shortUsed = 0;
            shortExpected = 0;
        }
    }

    private void beginShort(int status, int length) {
        shortMessage[0] = (byte)status;
        shortUsed = 1;
        shortExpected = length;
    }

    private static int channelLength(int status) {
        int command = status & 0xf0;
        return command == 0xc0 || command == 0xd0 ? 2 : 3;
    }

    private void endSysex() {
        inSysex = false;
        discardingSysex = false;
        sysexUsed = 0;
    }
}
