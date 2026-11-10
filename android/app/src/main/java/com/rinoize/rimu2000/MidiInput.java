package com.rinoize.rimu2000;

import android.content.Context;
import android.media.midi.MidiDevice;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiManager;
import android.media.midi.MidiOutputPort;
import android.media.midi.MidiReceiver;
import android.os.Handler;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicLong;

/** Device bookkeeping lives on the main thread; MIDI bytes go straight to JNI. */
final class MidiInput {
    interface Listener {
        void devicesChanged();
        void connectionChanged(String text);
    }

    static final class Endpoint {
        final MidiDeviceInfo device;
        final int port;
        final String title;

        Endpoint(MidiDeviceInfo device, int port, String title) {
            this.device = device; this.port = port; this.title = title;
        }

        String key() { return device.getId() + ":" + port; }
        @Override public String toString() { return title; }
    }

    private final MidiManager manager;
    private final Handler handler;
    private final Listener listener;
    private final AtomicLong received = new AtomicLong();
    private final Object receiveGate = new Object();
    private final MidiManager.DeviceCallback callback;
    private MidiDevice opened;
    private MidiOutputPort output;
    private volatile int generation;
    private int openedId = -1;
    private volatile boolean accepting;
    private boolean registered;
    private volatile MidiStreamParser activeParser;

    MidiInput(Context context, Handler handler, Listener listener) {
        this.manager = (MidiManager) context.getSystemService(Context.MIDI_SERVICE);
        this.handler = handler;
        this.listener = listener;
        callback = new MidiManager.DeviceCallback() {
            @Override public void onDeviceAdded(MidiDeviceInfo info) {
                listener.devicesChanged();
            }
            @Override public void onDeviceRemoved(MidiDeviceInfo info) {
                if (info.getId() == openedId) {
                    close();
                    if (NativeEngine.LOAD_ERROR == null) NativeEngine.panic();
                    listener.connectionChanged("Dispositivo MIDI desconectado.");
                }
                listener.devicesChanged();
            }
        };
    }

    void resume() {
        if (manager != null && !registered) {
            manager.registerDeviceCallback(callback, handler);
            registered = true;
        }
    }

    void pause() {
        close();
        if (manager != null && registered) {
            manager.unregisterDeviceCallback(callback);
            registered = false;
        }
    }

    List<Endpoint> endpoints() {
        List<Endpoint> out = new ArrayList<>();
        if (manager == null) return out;
        for (MidiDeviceInfo device : manager.getDevices()) {
            String name = device.getProperties().getString(MidiDeviceInfo.PROPERTY_NAME);
            if (name == null || name.isEmpty()) {
                name = device.getProperties().getString(MidiDeviceInfo.PROPERTY_PRODUCT,
                        "MIDI " + device.getId());
            }
            for (MidiDeviceInfo.PortInfo port : device.getPorts()) {
                if (port.getType() != MidiDeviceInfo.PortInfo.TYPE_OUTPUT) continue;
                String portName = port.getName();
                if (portName == null || portName.isEmpty()) portName = "Salida " + (port.getPortNumber() + 1);
                out.add(new Endpoint(device, port.getPortNumber(), name + " · " + portName));
            }
        }
        return out;
    }

    void connect(Endpoint endpoint) {
        close();
        if (manager == null || endpoint == null) {
            listener.connectionChanged("No hay entrada MIDI disponible.");
            return;
        }
        final int request = generation;
        openedId = endpoint.device.getId();
        listener.connectionChanged("Conectando " + endpoint.title + "…");
        try {
            manager.openDevice(endpoint.device, device -> {
                if (request != generation || !registered) {
                    closeQuietly(device);
                    return;
                }
                if (device == null) {
                    openedId = -1;
                    listener.connectionChanged("No se pudo abrir la entrada MIDI.");
                    return;
                }
                MidiOutputPort port;
                try {
                    port = device.openOutputPort(endpoint.port);
                } catch (RuntimeException e) {
                    closeQuietly(device);
                    openedId = -1;
                    listener.connectionChanged("No se pudo abrir el puerto MIDI: " + e.getMessage());
                    return;
                }
                if (port == null) {
                    closeQuietly(device);
                    openedId = -1;
                    listener.connectionChanged("El puerto MIDI no está disponible.");
                    return;
                }
                opened = device;
                output = port;
                accepting = true;
                received.set(0);
                final MidiStreamParser parser = new MidiStreamParser(
                        (data, offset, count) -> NativeEngine.sendMidi(data, offset, count, 0));
                activeParser = parser;
                port.connect(new MidiReceiver() {
                    @Override public void onSend(byte[] data, int offset, int count, long timestamp) {
                        synchronized (receiveGate) {
                            if (!accepting || request != generation || NativeEngine.LOAD_ERROR != null) return;
                            parser.accept(data, offset, count);
                            received.addAndGet(count);
                        }
                    }
                });
                listener.connectionChanged(endpoint.title + " · conectado (canales originales)");
            }, handler);
        } catch (RuntimeException e) {
            close();
            listener.connectionChanged("No se pudo conectar MIDI: " + e.getMessage());
        }
    }

    boolean isConnected() { return output != null; }
    long receivedBytes() { return received.get(); }

    String diagnostics() {
        MidiStreamParser parser = activeParser;
        String text = " · " + receivedBytes() + " bytes recibidos";
        if (parser == null) return text;
        if (parser.oversizedSysex() > 0) text += " · SysEx descartados (>64 KiB): " + parser.oversizedSysex();
        if (parser.interruptedMessages() > 0) text += " · mensajes incompletos: " + parser.interruptedMessages();
        if (parser.strayBytes() > 0) text += " · bytes sin mensaje: " + parser.strayBytes();
        return text;
    }

    void close() {
        synchronized (receiveGate) {
            accepting = false;
            generation++;
        }
        openedId = -1;
        closeQuietly(output);
        output = null;
        closeQuietly(opened);
        opened = null;
    }

    private static void closeQuietly(java.io.Closeable closeable) {
        if (closeable == null) return;
        try { closeable.close(); } catch (IOException ignored) { }
    }
}
