package com.rinoize.rimu2000;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.media.AudioAttributes;
import android.media.AudioDeviceCallback;
import android.media.AudioDeviceInfo;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputFilter;
import android.text.InputType;
import android.text.method.LinkMovementMethod;
import android.text.util.Linkify;
import android.view.Gravity;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Standalone first port: Android routing/UI around the unchanged MU2000 engine. */
public final class MainActivity extends Activity {
    private static final int PICK_FILES = 100, PICK_FOLDER = 101;
    private static final int BG = 0xff0c121c, CARD = 0xff151f2c;
    private static final int INK = 0xffe6edf5, MUTED = 0xffa0afc1, ACCENT = 0xff7fd5ff;
    private static final int[] BUFFERS = {512, 1024, 2048};

    private final Handler main = new Handler(Looper.getMainLooper());
    // Shared across Activity instances so a retiring screen cannot stop a new
    // screen's engine after a configuration change.
    private static final ExecutorService worker = Executors.newSingleThreadExecutor();
    private volatile boolean visible;
    private volatile boolean destroyed;
    private volatile int generation;
    private boolean busy, starting, running, audioRegistered;
    private int uiChannel;
    private String midiConnectionText = "MIDI desconectado";
    private AudioManager audio;
    private AudioFocusRequest focus;
    private SharedPreferences preferences;
    private File roms, state;
    private MidiInput midi;
    private final List<MidiInput.Endpoint> midiEndpoints = new ArrayList<>();
    private final List<AudioChoice> audioChoices = new ArrayList<>();

    private TextView status, diagnostics, romStatus, midiStatus, gainLabel;
    private Button start, stop, panic, importFiles, importFolder, connectMidi, refreshMidi, applyProgram;
    private Spinner outputPicker, bufferPicker, midiPicker, channelPicker;
    private EditText bankMsb, bankLsb, program;
    private Switch sustain;
    private KeyboardView keyboard;
    private SeekBar gain;

    private final AudioDeviceCallback audioCallback = new AudioDeviceCallback() {
        @Override public void onAudioDevicesAdded(AudioDeviceInfo[] addedDevices) { refreshAudio(); }
        @Override public void onAudioDevicesRemoved(AudioDeviceInfo[] removedDevices) {
            AudioChoice selected = selectedOutput();
            if (selected != null && (running || starting)) {
                for (AudioDeviceInfo removed : removedDevices) {
                    if (removed.getId() == selected.id) {
                        requestStop("Salida de audio desconectada. Elegí otra salida e iniciá nuevamente.");
                        break;
                    }
                }
            }
            refreshAudio();
        }
    };

    private final Runnable poll = new Runnable() {
        @Override public void run() {
            if (destroyed || !visible) return;
            if (NativeEngine.LOAD_ERROR == null && !busy) {
                boolean active = NativeEngine.isRunning();
                if (running && !active) {
                    requestStop("Salida de audio detenida o desconectada. "
                            + "Elegí la salida e iniciá nuevamente.");
                }
                running = active;
                diagnostics.setText(NativeEngine.stats());
                updateControls();
            }
            if (midi != null && midi.isConnected()) {
                midiStatus.setText(midiConnectionText + midi.diagnostics());
            }
            main.postDelayed(this, 1000);
        }
    };

    @Override public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        audio = (AudioManager) getSystemService(AUDIO_SERVICE);
        preferences = getSharedPreferences("settings", MODE_PRIVATE);
        roms = new File(getFilesDir(), "roms");
        state = new File(getFilesDir(), "state");
        roms.mkdirs(); state.mkdirs();
        focus = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setAcceptsDelayedFocusGain(false)
                .setOnAudioFocusChangeListener(change -> {
                    if (change == AudioManager.AUDIOFOCUS_LOSS
                            || change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT
                            || change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK) {
                        requestStop("Audio pausado por otra aplicación. Tocá Iniciar para continuar.");
                    }
                }, main).build();
        buildUi();
        midi = new MidiInput(this, main, new MidiInput.Listener() {
            @Override public void devicesChanged() { refreshMidi(); }
            @Override public void connectionChanged(String text) {
                if (!destroyed) {
                    midiConnectionText = text;
                    midiStatus.setText(text);
                    connectMidi.setText(midi.isConnected() ? "Desconectar" : "Conectar");
                }
            }
        });
        refreshAudio();
        refreshMidi();
        refreshRoms();
        if (NativeEngine.LOAD_ERROR != null) status.setText(NativeEngine.LOAD_ERROR);
        updateControls();
    }

    @Override protected void onResume() {
        super.onResume();
        visible = true;
        midi.resume();
        refreshMidi();
        refreshAudio();
        if (!audioRegistered) {
            audio.registerAudioDeviceCallback(audioCallback, main);
            audioRegistered = true;
        }
        main.removeCallbacks(poll);
        main.post(poll);
    }

    @Override protected void onStop() {
        visible = false;
        main.removeCallbacks(poll);
        midi.pause();
        midiStatus.setText("MIDI desconectado. Elegí el puerto y tocá Conectar.");
        connectMidi.setText("Conectar");
        if (audioRegistered) {
            audio.unregisterAudioDeviceCallback(audioCallback);
            audioRegistered = false;
        }
        requestStop("Audio detenido al salir de la aplicación.");
        super.onStop();
    }

    @Override protected void onDestroy() {
        destroyed = true;
        generation++;
        main.removeCallbacksAndMessages(null);
        midi.pause();
        // onStop already cancelled and queued the engine shutdown. A late
        // onDestroy from an older Activity must not stop a newer screen's engine.
        super.onDestroy();
    }

    private void buildUi() {
        getWindow().setStatusBarColor(BG);
        getWindow().setNavigationBarColor(BG);
        if (Build.VERSION.SDK_INT >= 30) getWindow().setDecorFitsSystemWindows(false);
        else getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(BG);
        scroll.setClipToPadding(false);
        scroll.setOnApplyWindowInsetsListener((v, insets) -> {
            int left, top, right, bottom;
            if (Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars()
                        | WindowInsets.Type.displayCutout() | WindowInsets.Type.ime());
                left = bars.left; top = bars.top; right = bars.right; bottom = bars.bottom;
            } else {
                left = insets.getSystemWindowInsetLeft(); top = insets.getSystemWindowInsetTop();
                right = insets.getSystemWindowInsetRight(); bottom = insets.getSystemWindowInsetBottom();
            }
            v.setPadding(left + dp(16), top + dp(16), right + dp(16), bottom + dp(20));
            return insets;
        });
        LinearLayout content = column();
        content.setPadding(0, 0, 0, dp(8));
        scroll.addView(content, new ScrollView.LayoutParams(-1, -2));
        setContentView(scroll);
        scroll.requestApplyInsets();

        TextView title = text("RiMU2000", 30, INK);
        title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        content.addView(title);
        content.addView(text("YAMAHA MU2000 · MOTOR S-MU2000 · ALPHA 0.1.0", 11, MUTED));

        LinearLayout display = card(content, 0xff0d2b43);
        TextView displayTitle = text("MU2000  /  ANDROID", 13, ACCENT);
        displayTitle.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        display.addView(displayTitle);
        status = text("Importá las ROMs, conectá tu teclado e iniciá el audio.", 17, INK);
        status.setPadding(0, dp(8), 0, dp(10));
        display.addView(status);
        diagnostics = text("Motor ARM64 · firmware original · 44,1 kHz internos", 12, ACCENT);
        diagnostics.setTypeface(Typeface.MONOSPACE);
        diagnostics.setTextIsSelectable(true);
        display.addView(diagnostics);
        LinearLayout transport = row();
        display.addView(transport);
        start = button("Iniciar", v -> startAudio());
        start.setBackgroundTintList(ColorStateList.valueOf(0xff2278a6));
        stop = button("Detener", v -> requestStop("Audio detenido."));
        panic = button("Panic", v -> { releaseNotes(); NativeEngine.panic(); });
        weighted(transport, start, 1); weighted(transport, stop, 1); weighted(transport, panic, 1);

        LinearLayout routing = card(content, CARD);
        heading(routing, "AUDIO");
        LinearLayout audioRow = row();
        routing.addView(audioRow);
        LinearLayout outputCol = labelled(audioRow, "Salida", 3);
        outputPicker = spinner(); outputCol.addView(outputPicker);
        LinearLayout bufferCol = labelled(audioRow, "Buffer solicitado", 2);
        bufferPicker = spinner(); bufferCol.addView(bufferPicker);
        setItems(bufferPicker, new String[]{"512 frames", "1024 frames", "2048 frames"});
        int savedBuffer = preferences.getInt("buffer", 1024);
        bufferPicker.setSelection(savedBuffer == 512 ? 0 : savedBuffer == 2048 ? 2 : 1);
        TextView audioHelp = text("Empezá con 1024. El diagnóstico muestra el buffer que Android aceptó. "
                + "Para cambiar la salida o el buffer, detené el audio.", 12, MUTED);
        audioHelp.setPadding(0, dp(7), 0, 0);
        routing.addView(audioHelp);
        gainLabel = text("Volumen de salida", 13, INK);
        gainLabel.setPadding(0, dp(12), 0, 0); routing.addView(gainLabel);
        gain = new SeekBar(this); gain.setMax(100); gain.setProgress(preferences.getInt("gain", 80));
        routing.addView(gain, new LinearLayout.LayoutParams(-1, dp(42)));
        gain.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar bar, int value, boolean fromUser) {
                gainLabel.setText("Volumen de salida · " + value + "%");
                if (NativeEngine.LOAD_ERROR == null) NativeEngine.setGain(value / 100f);
                if (fromUser) preferences.edit().putInt("gain", value).apply();
            }
            public void onStartTrackingTouch(SeekBar bar) {}
            public void onStopTrackingTouch(SeekBar bar) {}
        });
        gainLabel.setText("Volumen de salida · " + gain.getProgress() + "%");

        LinearLayout midiCard = card(content, CARD);
        heading(midiCard, "ENTRADA MIDI");
        midiPicker = spinner(); midiCard.addView(midiPicker);
        LinearLayout midiButtons = row(); midiCard.addView(midiButtons);
        connectMidi = button("Conectar", v -> {
            if (midi.isConnected()) {
                midi.close(); NativeEngine.panic();
                midiStatus.setText("MIDI desconectado."); connectMidi.setText("Conectar");
            } else {
                int index = midiPicker.getSelectedItemPosition();
                if (index >= 0 && index < midiEndpoints.size()) midi.connect(midiEndpoints.get(index));
            }
        });
        refreshMidi = button("Actualizar", v -> refreshMidi());
        weighted(midiButtons, connectMidi, 1); weighted(midiButtons, refreshMidi, 1);
        midiStatus = text("Conectá un teclado USB-MIDI y elegí su puerto.", 12, MUTED);
        midiCard.addView(midiStatus);
        midiCard.addView(text("Se conservan los canales, sustain, pitch bend y SysEx del teclado.", 12, MUTED));

        LinearLayout voice = card(content, CARD);
        heading(voice, "SONIDO Y TECLADO EN PANTALLA");
        LinearLayout voiceFields = row(); voice.addView(voiceFields);
        LinearLayout channels = labelled(voiceFields, "Canal", 1);
        channelPicker = spinner(); channels.addView(channelPicker);
        String[] channelNames = new String[16];
        for (int i = 0; i < 16; i++) channelNames[i] = String.valueOf(i + 1);
        setItems(channelPicker, channelNames);
        bankMsb = number(labelled(voiceFields, "MSB", 1), preferences.getInt("msb", 0));
        bankLsb = number(labelled(voiceFields, "LSB", 1), preferences.getInt("lsb", 0));
        program = number(labelled(voiceFields, "Programa", 1), preferences.getInt("program", 1));
        LinearLayout voiceButtons = row(); voice.addView(voiceButtons);
        applyProgram = button("Aplicar sonido", v -> applyVoice());
        weighted(voiceButtons, applyProgram, 1);
        sustain = new Switch(this); sustain.setText("Sustain"); sustain.setTextColor(INK);
        sustain.setPadding(dp(12), 0, dp(8), 0);
        weighted(voiceButtons, sustain, 1);
        sustain.setOnCheckedChangeListener((button, checked) -> send(0xb0 | uiChannel, 64, checked ? 127 : 0));
        channelPicker.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                releaseNotes();
                uiChannel = position;
            }
            public void onNothingSelected(AdapterView<?> parent) {}
        });
        voice.addView(text("MSB/LSB: 0–127 · Programa: 1–128. El canal afecta solo estos controles; "
                + "la entrada USB conserva su canal.", 12, MUTED));
        keyboard = new KeyboardView(this, new KeyboardView.Notes() {
            public void on(int note) { send(0x90 | uiChannel, note, 100); }
            public void off(int note) { send(0x80 | uiChannel, note, 0); }
        });
        LinearLayout.LayoutParams keys = new LinearLayout.LayoutParams(-1, dp(130));
        keys.topMargin = dp(14); voice.addView(keyboard, keys);

        LinearLayout romCard = card(content, CARD);
        heading(romCard, "ROMs DEL MU2000");
        romStatus = text("", 13, INK); romCard.addView(romStatus);
        LinearLayout romButtons = row(); romCard.addView(romButtons);
        importFiles = button("Importar archivos", v -> pick(false));
        importFolder = button("Importar carpeta", v -> pick(true));
        weighted(romButtons, importFiles, 1); weighted(romButtons, importFolder, 1);
        romCard.addView(text("Seleccioná mu2000_flash.bin (4 MiB) y las cuatro ROMs de ondas "
                + "(8 MiB cada una). Podés agregarlas en varias importaciones. También admite "
                + "sin-table.bin como archivo opcional. Los ZIP deben descomprimirse antes.",
                12, MUTED));
        TextView footer = text("Proyecto no oficial, sin relación con Yamaha. Basado en S-MU2000 de tarboh. "
                + "La aplicación no incluye firmware ni ondas. Esta alpha se detiene al salir; no se inicia sola al volver.",
                11, MUTED);
        footer.setPadding(dp(2), dp(18), dp(2), 0); content.addView(footer);
        Button about = button("Acerca de / Licencias", v -> showLicenses());
        LinearLayout.LayoutParams aboutParams = new LinearLayout.LayoutParams(-1, -2);
        aboutParams.topMargin = dp(10); content.addView(about, aboutParams);
    }

    private void showLicenses() {
        worker.execute(() -> {
            StringBuilder contents = new StringBuilder("RiMU2000 Android · alpha 0.1.0\n\n")
                    .append("Adaptación para Android del proyecto S-MU2000 de tarboh.\n")
                    .append("Proyecto no oficial, sin relación con Yamaha.\n\n")
                    .append("Código fuente del port:\nhttps://github.com/RiNoize/RiMU2000-Android\n\n")
                    .append("Proyecto original:\nhttps://github.com/tarboh/S-MU2000\n\n");
            for (String name : new String[]{"LICENSE", "NOTICE.txt"}) {
                contents.append("──────── ").append(name).append(" ────────\n\n");
                try (InputStreamReader reader = new InputStreamReader(
                        getAssets().open("licenses/" + name), StandardCharsets.UTF_8)) {
                    char[] buffer = new char[4096];
                    int read;
                    while ((read = reader.read(buffer)) != -1) contents.append(buffer, 0, read);
                } catch (IOException e) {
                    contents.append("No se pudo abrir el aviso: ").append(e.getMessage());
                }
                contents.append("\n\n");
            }
            final String notices = contents.toString();
            main.post(() -> {
                if (destroyed || !visible) return;
                TextView text = text(notices, 13, INK);
                text.setPadding(dp(18), dp(14), dp(18), dp(14));
                text.setTextIsSelectable(true);
                Linkify.addLinks(text, Linkify.WEB_URLS);
                text.setMovementMethod(LinkMovementMethod.getInstance());
                ScrollView scroll = new ScrollView(this);
                scroll.addView(text);
                new AlertDialog.Builder(this).setTitle("Acerca de RiMU2000")
                        .setView(scroll).setPositiveButton("Cerrar", null).show();
            });
        });
    }

    private void startAudio() {
        if (busy || running || NativeEngine.LOAD_ERROR != null || !visible) return;
        List<String> missing = RomImporter.missing(roms);
        if (!missing.isEmpty()) {
            status.setText("Primero importá las ROMs que faltan.");
            new AlertDialog.Builder(this).setTitle("ROMs necesarias")
                    .setMessage(String.join("\n", missing)).setPositiveButton("Aceptar", null).show();
            return;
        }
        if (audio.requestAudioFocus(focus) != AudioManager.AUDIOFOCUS_REQUEST_GRANTED) {
            status.setText("Otra aplicación está usando el audio. Cerrala e intentá nuevamente.");
            return;
        }
        final int request = ++generation;
        final int frames = BUFFERS[Math.max(0, bufferPicker.getSelectedItemPosition())];
        AudioChoice selected = selectedOutput();
        final int deviceId = selected == null ? 0 : selected.id;
        preferences.edit().putInt("buffer", frames).apply();
        busy = true; starting = true;
        status.setText("Iniciando el MU2000… la primera carga puede tardar unos segundos.");
        updateControls();
        NativeEngine.setGain(gain.getProgress() / 100f);
        worker.execute(() -> {
            if (destroyed || !visible || request != generation) return;
            String result;
            try {
                result = NativeEngine.start(roms.getAbsolutePath(), state.getAbsolutePath(), frames, deviceId);
            } catch (RuntimeException | LinkageError e) {
                result = "No se pudo iniciar: " + e.getMessage();
            }
            if (destroyed || !visible || request != generation) {
                NativeEngine.stop();
                return;
            }
            final String error = result;
            main.post(() -> {
                if (destroyed || request != generation) return;
                busy = false; starting = false;
                running = error != null && error.isEmpty() && NativeEngine.isRunning();
                if (running) {
                    status.setText("Listo para tocar · USB-MIDI y teclado en pantalla");
                    applyVoice();
                } else {
                    status.setText(error == null || error.isEmpty() ? "El motor no pudo iniciar." : error);
                    abandonFocus();
                }
                diagnostics.setText(NativeEngine.stats());
                updateControls();
            });
        });
    }

    private void requestStop(String message) {
        if (destroyed) return;
        releaseNotes();
        final int request = ++generation;
        if (NativeEngine.LOAD_ERROR != null) { busy = false; running = false; updateControls(); return; }
        NativeEngine.requestStop();
        busy = true; starting = false; running = false;
        status.setText(message);
        abandonFocus();
        updateControls();
        worker.execute(() -> {
            NativeEngine.stop();
            main.post(() -> {
                if (destroyed || request != generation) return;
                busy = false; running = false;
                diagnostics.setText(NativeEngine.stats());
                refreshRoms();
                updateControls();
            });
        });
    }

    private void abandonFocus() { if (audio != null && focus != null) audio.abandonAudioFocusRequest(focus); }

    private void releaseNotes() {
        if (keyboard != null) keyboard.releaseAll();
        if (sustain != null && sustain.isChecked()) sustain.setChecked(false);
    }

    private void applyVoice() {
        if (!running) return;
        Integer msb = bounded(bankMsb, 0, 127), lsb = bounded(bankLsb, 0, 127);
        Integer pc = bounded(program, 1, 128);
        if (msb == null || lsb == null || pc == null) return;
        releaseNotes();
        byte[] bytes = {(byte)(0xb0 | uiChannel), 0, msb.byteValue(),
                (byte)(0xb0 | uiChannel), 32, lsb.byteValue(),
                (byte)(0xc0 | uiChannel), (byte)(pc - 1)};
        NativeEngine.sendMidi(bytes, 0, bytes.length, 0);
        preferences.edit().putInt("msb", msb).putInt("lsb", lsb).putInt("program", pc).apply();
    }

    private Integer bounded(EditText field, int min, int max) {
        try {
            int value = Integer.parseInt(field.getText().toString());
            if (value >= min && value <= max) { field.setError(null); return value; }
        } catch (NumberFormatException ignored) { }
        field.setError("Usá " + min + " a " + max);
        return null;
    }

    private void send(int statusByte, int a, int b) {
        if (NativeEngine.LOAD_ERROR != null) return;
        byte[] bytes = {(byte) statusByte, (byte) a, (byte) b};
        NativeEngine.sendMidi(bytes, 0, bytes.length, 0);
    }

    private void pick(boolean folder) {
        if (busy || running) return;
        Intent intent = new Intent(folder ? Intent.ACTION_OPEN_DOCUMENT_TREE : Intent.ACTION_OPEN_DOCUMENT);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        if (!folder) {
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        }
        try {
            startActivityForResult(intent, folder ? PICK_FOLDER : PICK_FILES);
        } catch (RuntimeException e) {
            status.setText("No se pudo abrir el selector de archivos: " + e.getMessage());
        }
    }

    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null
                || (requestCode != PICK_FILES && requestCode != PICK_FOLDER)) return;
        final List<Uri> selected = new ArrayList<>();
        if (data.getClipData() != null) {
            ClipData clips = data.getClipData();
            for (int i = 0; i < Math.min(clips.getItemCount(), 50); i++) selected.add(clips.getItemAt(i).getUri());
        } else if (data.getData() != null) selected.add(data.getData());
        if (selected.isEmpty()) return;
        final int request = ++generation;
        busy = true;
        status.setText("Comprobando e importando ROMs…");
        updateControls();
        worker.execute(() -> {
            String report = requestCode == PICK_FOLDER
                    ? RomImporter.importTree(getContentResolver(), selected.get(0), roms)
                    : RomImporter.importFiles(getContentResolver(), selected, roms);
            main.post(() -> {
                if (destroyed || request != generation) return;
                busy = false;
                refreshRoms();
                status.setText(RomImporter.missing(roms).isEmpty()
                        ? "ROMs listas. Elegí la salida e iniciá el audio."
                        : "Importación terminada. Todavía faltan ROMs.");
                updateControls();
                if (visible) new AlertDialog.Builder(this).setTitle("Importación de ROMs")
                        .setMessage(report).setPositiveButton("Aceptar", null).show();
            });
        });
    }

    private void refreshRoms() { if (romStatus != null) romStatus.setText(RomImporter.summary(roms)); }

    private void refreshMidi() {
        if (midi == null || midiPicker == null) return;
        String previous = null;
        int old = midiPicker.getSelectedItemPosition();
        if (old >= 0 && old < midiEndpoints.size()) previous = midiEndpoints.get(old).key();
        midiEndpoints.clear(); midiEndpoints.addAll(midi.endpoints());
        List<String> labels = new ArrayList<>();
        int selected = 0;
        for (int i = 0; i < midiEndpoints.size(); i++) {
            labels.add(midiEndpoints.get(i).title);
            if (midiEndpoints.get(i).key().equals(previous)) selected = i;
        }
        if (labels.isEmpty()) labels.add("Sin dispositivos MIDI disponibles");
        setItems(midiPicker, labels.toArray(new String[0]));
        midiPicker.setSelection(selected);
        midiPicker.setEnabled(!midiEndpoints.isEmpty());
        connectMidi.setEnabled(!midiEndpoints.isEmpty() && NativeEngine.LOAD_ERROR == null);
        connectMidi.setText(midi.isConnected() ? "Desconectar" : "Conectar");
    }

    private void refreshAudio() {
        if (outputPicker == null || audio == null) return;
        AudioChoice old = selectedOutput();
        int wanted = old == null ? -1 : old.id;
        audioChoices.clear();
        AudioDeviceInfo[] devices = audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS);
        // Wired output wins on the first scan. USB is available but never preferred
        // over the tablet's own speaker/headphone path.
        int preferred = -1, speaker = -1;
        for (AudioDeviceInfo device : devices) {
            int type = device.getType();
            String kind;
            if (type == AudioDeviceInfo.TYPE_WIRED_HEADPHONES || type == AudioDeviceInfo.TYPE_WIRED_HEADSET) {
                kind = "Auriculares de la tablet"; preferred = audioChoices.size();
            } else if (type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER) {
                kind = "Altavoz de la tablet"; speaker = audioChoices.size();
            } else if (type == AudioDeviceInfo.TYPE_USB_DEVICE || type == AudioDeviceInfo.TYPE_USB_HEADSET
                    || type == AudioDeviceInfo.TYPE_USB_ACCESSORY) {
                kind = "USB: " + device.getProductName();
            } else if (type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP
                    || type == AudioDeviceInfo.TYPE_BLE_HEADSET || type == AudioDeviceInfo.TYPE_BLE_SPEAKER) {
                kind = "Bluetooth: " + device.getProductName();
            } else continue;
            audioChoices.add(new AudioChoice(device.getId(), kind));
        }
        if (audioChoices.isEmpty()) audioChoices.add(new AudioChoice(0, "Salida predeterminada de Android"));
        int chosen = preferred >= 0 ? preferred : speaker >= 0 ? speaker : 0;
        String[] names = new String[audioChoices.size()];
        for (int i = 0; i < names.length; i++) {
            names[i] = audioChoices.get(i).label;
            if (audioChoices.get(i).id == wanted) chosen = i;
        }
        setItems(outputPicker, names); outputPicker.setSelection(chosen);
        outputPicker.setEnabled(!busy && !running);
    }

    private AudioChoice selectedOutput() {
        if (outputPicker == null) return null;
        int index = outputPicker.getSelectedItemPosition();
        return index >= 0 && index < audioChoices.size() ? audioChoices.get(index) : null;
    }

    private void updateControls() {
        if (start == null) return;
        boolean available = NativeEngine.LOAD_ERROR == null;
        start.setEnabled(available && !busy && !running);
        stop.setEnabled(available && (running || starting));
        panic.setEnabled(available && running);
        outputPicker.setEnabled(!busy && !running);
        bufferPicker.setEnabled(!busy && !running);
        importFiles.setEnabled(!busy && !running);
        importFolder.setEnabled(!busy && !running);
        applyProgram.setEnabled(available && running);
        keyboard.setEnabled(available && running);
        sustain.setEnabled(available && running);
        gain.setEnabled(available);
        if (running || busy) getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    private static final class AudioChoice {
        final int id; final String label;
        AudioChoice(int id, String label) { this.id = id; this.label = label; }
    }

    private int dp(int value) { return (int)(value * getResources().getDisplayMetrics().density + .5f); }
    private LinearLayout column() {
        LinearLayout view = new LinearLayout(this); view.setOrientation(LinearLayout.VERTICAL); return view;
    }
    private LinearLayout row() {
        LinearLayout view = new LinearLayout(this); view.setOrientation(LinearLayout.HORIZONTAL);
        view.setGravity(Gravity.CENTER_VERTICAL); return view;
    }
    private TextView text(String value, int size, int color) {
        TextView view = new TextView(this); view.setText(value); view.setTextSize(size); view.setTextColor(color);
        view.setLineSpacing(dp(3), 1f); return view;
    }
    private LinearLayout card(LinearLayout parent, int color) {
        LinearLayout view = column(); view.setPadding(dp(15), dp(14), dp(15), dp(14));
        GradientDrawable drawable = new GradientDrawable(); drawable.setColor(color);
        drawable.setCornerRadius(dp(12)); drawable.setStroke(dp(1), 0xff28384b); view.setBackground(drawable);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
        params.topMargin = dp(14); parent.addView(view, params); return view;
    }
    private void heading(LinearLayout parent, String title) {
        TextView label = text(title, 12, ACCENT); label.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        label.setPadding(0, 0, 0, dp(9)); parent.addView(label);
    }
    private void weighted(LinearLayout parent, View child, int weight) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0, -2, weight);
        params.setMargins(dp(2), dp(5), dp(2), dp(3)); parent.addView(child, params);
    }
    private LinearLayout labelled(LinearLayout parent, String label, int weight) {
        LinearLayout column = column(); weighted(parent, column, weight);
        column.addView(text(label, 11, MUTED)); return column;
    }
    private Button button(String title, View.OnClickListener listener) {
        Button button = new Button(this); button.setText(title); button.setTextColor(INK);
        button.setTextSize(13); button.setAllCaps(false); button.setMinHeight(dp(48));
        button.setPadding(dp(8), dp(4), dp(8), dp(4));
        button.setBackgroundTintList(ColorStateList.valueOf(0xff2c3b4e));
        button.setOnClickListener(listener); return button;
    }
    private Spinner spinner() {
        Spinner spinner = new Spinner(this, Spinner.MODE_DIALOG);
        spinner.setMinimumHeight(dp(48)); spinner.setPopupBackgroundDrawable(new android.graphics.drawable.ColorDrawable(CARD));
        spinner.setLayoutParams(new LinearLayout.LayoutParams(-1, dp(48))); return spinner;
    }
    private void setItems(Spinner spinner, String[] values) {
        ArrayAdapter<String> adapter = new ArrayAdapter<String>(this,
                android.R.layout.simple_spinner_item, values);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinner.setAdapter(adapter);
    }
    private EditText number(LinearLayout parent, int value) {
        EditText text = new EditText(this); text.setText(String.valueOf(value));
        text.setSingleLine(); text.setTextColor(INK); text.setTextSize(16);
        text.setSelectAllOnFocus(true); text.setInputType(InputType.TYPE_CLASS_NUMBER);
        text.setFilters(new InputFilter[]{new InputFilter.LengthFilter(3)});
        parent.addView(text, new LinearLayout.LayoutParams(-1, dp(48))); return text;
    }
}
