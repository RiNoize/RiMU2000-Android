package com.rinoize.rimu2000;

/** Small JNI surface; the native audio callback owns the emulated machine. */
public final class NativeEngine {
    public static final String LOAD_ERROR;
    static {
        String error = null;
        try {
            System.loadLibrary("rimu2000");
        } catch (LinkageError e) {
            error = "No se pudo cargar el motor ARM64: " + e.getMessage();
        }
        LOAD_ERROR = error;
    }

    private NativeEngine() {}

    /** Returns an empty string on success; call only from a lifecycle worker. */
    public static native String start(String romDir, String stateDir,
                                      int bufferFrames, int deviceId, int cpuMode);
    public static native void stop();
    public static native void requestStop();
    public static native void sendMidi(byte[] data, int offset, int count, int port);
    public static native void panic();
    public static native void setGain(float gain);
    public static native String stats();
    /** Snapshot of firmware state: [voice names, part/bank/program/routing details]. */
    public static native String[] currentSound(int channel, boolean followProgramChanges);
    /** The retained slow callbacks; safe to read while audio is running or stopped. */
    public static native String callbackDiagnostics();
    /** Enabling starts a fresh capture with scheduling detail; disabling retains records. */
    public static native void setDetailedDiagnostics(boolean enabled);
    public static native boolean isRunning();
}
