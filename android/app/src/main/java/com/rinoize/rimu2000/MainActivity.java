package com.rinoize.rimu2000;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
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
import android.os.SystemClock;
import android.text.InputFilter;
import android.text.InputType;
import android.text.TextUtils;
import android.text.method.LinkMovementMethod;
import android.text.util.Linkify;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.GridLayout;
import android.widget.LinearLayout;
import android.widget.PopupMenu;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** Standalone first port: Android routing/UI around the unchanged MU2000 engine. */
public final class MainActivity extends Activity {
    private static final int PICK_FILES = 100, PICK_FOLDER = 101;
    private static final int BG = 0xff0c121c, CARD = 0xff151f2c;
    private static final int INK = 0xffe6edf5, MUTED = 0xffa0afc1, ACCENT = 0xff7fd5ff;
    private static final int[] BUFFERS = {512, 1024, 2048};
    private static final String APP_VERSION = "0.1.4";
    private static final String[] FAMILIES = {
            "Pianos", "Percusión cromática", "Órganos", "Guitarras", "Bajos", "Cuerdas",
            "Conjuntos", "Metales", "Maderas", "Flautas", "Solistas de sintetizador", "Pads",
            "Efectos de sintetizador", "Étnicos", "Percusión", "Efectos de sonido", "Baterías", "SFX y efectos"
    };

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
    private TextView soundName, soundDetails, callbackTrace;
    private Button start, stop, panic, importFiles, importFolder, connectMidi, refreshMidi, applyProgram;
    private Button copyDiagnostics;
    private Spinner outputPicker, bufferPicker, cpuPicker, midiPicker, channelPicker;
    private EditText bankMsb, bankLsb, program;
    private Switch sustain, followProgramChanges, detailedDiagnostics;
    private LinearLayout cutDetails;
    private KeyboardView keyboard;
    private SeekBar gain;
    private LinearLayout instrumentRoot, instrumentBody, keyboardPanel;
    private ScrollView performanceScroll;
    private GridLayout toneGrid;
    private Button keyboardToggle;
    private PatchBrowserView browser;
    private final List<PartControlView> partControls = new ArrayList<>();
    private AlertDialog connectionsDialog, diagnosticsDialog, manualDialog;
    private boolean wideLayout, layoutReady, catalogLoading;
    private int toneColumns;
    private CatalogData catalog;
    private String catalogAttemptKey = "", snapshotCatalogKey = "", lastStateJson = "";
    private String pendingVoiceId = "", pendingVoiceName = "";
    private long pendingVoiceAt;
    private JSONObject soundState;

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
                refreshCallbackDiagnostics();
                updateControls();
            }
            if (midi != null && midi.isConnected()) {
                midiStatus.setText(midiConnectionText + midi.diagnostics());
            }
            main.postDelayed(this, 1000);
        }
    };

    // Sound state follows actual firmware changes independently of the slower
    // performance panel. This never changes routing, draft fields, or MIDI.
    private final Runnable soundPoll = new Runnable() {
        @Override public void run() {
            if (destroyed || !visible) return;
            refreshCurrentSound();
            main.postDelayed(this, 250);
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
        main.removeCallbacks(soundPoll);
        main.post(soundPoll);
    }

    @Override protected void onStop() {
        visible = false;
        main.removeCallbacks(poll);
        main.removeCallbacks(soundPoll);
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
        if (connectionsDialog != null) connectionsDialog.dismiss();
        if (diagnosticsDialog != null) diagnosticsDialog.dismiss();
        if (manualDialog != null) manualDialog.dismiss();
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
        instrumentRoot = column();
        instrumentRoot.setBackgroundColor(BG);
        instrumentRoot.setClipToPadding(true);
        instrumentRoot.setFocusableInTouchMode(true);
        instrumentRoot.setOnApplyWindowInsetsListener((v, insets) -> {
            int left, top, right, bottom;
            if (Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars()
                        | WindowInsets.Type.displayCutout() | WindowInsets.Type.ime());
                left = bars.left; top = bars.top; right = bars.right; bottom = bars.bottom;
            } else {
                left = insets.getSystemWindowInsetLeft(); top = insets.getSystemWindowInsetTop();
                right = insets.getSystemWindowInsetRight(); bottom = insets.getSystemWindowInsetBottom();
            }
            v.setPadding(left + dp(10), top + dp(6), right + dp(10), bottom + dp(8));
            return insets;
        });
        setContentView(instrumentRoot);
        instrumentRoot.requestApplyInsets();

        LinearLayout toolbar = row();
        LinearLayout brand = column();
        TextView title = text("RiMU2000", 22, INK);
        title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        brand.addView(title);
        brand.addView(text("MU2000 · alpha " + APP_VERSION, 10, MUTED));
        toolbar.addView(brand, new LinearLayout.LayoutParams(0, -2, 1));
        start = button("Iniciar", v -> startAudio());
        start.setBackgroundTintList(ColorStateList.valueOf(0xff2278a6));
        stop = button("Detener", v -> requestStop("Audio detenido."));
        panic = button("Panic", v -> { releaseNotes(); NativeEngine.panic(); });
        keyboardToggle = button("Teclas", v -> toggleKeyboard());
        Button menu = button("⋮", this::showMenu);
        menu.setTextSize(23); menu.setContentDescription("Conexiones, diagnóstico y opciones");
        toolbarButton(toolbar, start, 64); toolbarButton(toolbar, stop, 68);
        toolbarButton(toolbar, panic, 58); toolbarButton(toolbar, keyboardToggle, 64);
        toolbarButton(toolbar, menu, 40);
        instrumentRoot.addView(toolbar);

        LinearLayout display = column();
        display.setPadding(dp(13), dp(5), dp(13), dp(8));
        display.setBackground(panelBackground(0xff0d2b43));
        LinearLayout displayHeading = row();
        LinearLayout voiceIdentity = column();
        TextView lcdTitle = text("SONIDO ACTUAL", 11, ACCENT);
        lcdTitle.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        voiceIdentity.addView(lcdTitle);
        soundName = text("Tu MU2000, listo para empezar", 27, INK);
        soundName.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        soundName.setSingleLine(true); soundName.setEllipsize(TextUtils.TruncateAt.END);
        soundName.setContentDescription("Sonido actual confirmado por el MU2000. Tocar para ver todas las partes.");
        voiceIdentity.addView(soundName);
        displayHeading.addView(voiceIdentity, new LinearLayout.LayoutParams(0, -2, 1));
        followProgramChanges = new Switch(this);
        followProgramChanges.setText("Seguir Program Change");
        followProgramChanges.setTextColor(INK);
        followProgramChanges.setTextSize(12);
        followProgramChanges.setMinHeight(dp(48));
        followProgramChanges.setChecked(preferences.getBoolean("follow_program_changes", true));
        displayHeading.addView(followProgramChanges);
        display.addView(displayHeading);
        soundDetails = text("Importá las ROMs en Conexiones e iniciá el audio.", 12, ACCENT);
        soundDetails.setSingleLine(true); soundDetails.setEllipsize(TextUtils.TruncateAt.END);
        display.addView(soundDetails);
        soundName.setOnClickListener(v -> showSoundDetails());
        soundDetails.setOnClickListener(v -> showSoundDetails());
        status = text("Elegí un sonido en la lista y tocá desde tu teclado MIDI.", 11, MUTED);
        status.setMaxLines(2); status.setEllipsize(TextUtils.TruncateAt.END);
        display.addView(status);
        LinearLayout.LayoutParams lcdParams = new LinearLayout.LayoutParams(-1, -2);
        lcdParams.setMargins(0, dp(5), 0, dp(8)); instrumentRoot.addView(display, lcdParams);
        followProgramChanges.setOnCheckedChangeListener((button, checked) -> {
            preferences.edit().putBoolean("follow_program_changes", checked).apply();
            refreshCurrentSound();
        });

        instrumentBody = row();
        instrumentBody.setGravity(Gravity.TOP);
        browser = new PatchBrowserView(this, preferences, this::loadPatch);
        instrumentBody.addView(browser);
        performanceScroll = new ScrollView(this);
        performanceScroll.setFillViewport(true);
        performanceScroll.setBackground(panelBackground(CARD));
        LinearLayout performance = column();
        performance.setPadding(dp(10), dp(10), dp(10), dp(8));
        performance.addView(text("CONTROLES · PUERTO A", 11, ACCENT));
        LinearLayout playing = row();
        channelPicker = spinner();
        channelPicker.setPrompt("Canal de los controles en puerto A");
        channelPicker.setContentDescription("Canal MIDI de los controles en pantalla");
        String[] channelNames = new String[16];
        for (int i = 0; i < 16; i++) channelNames[i] = "A" + (i + 1);
        setItems(channelPicker, channelNames);
        playing.addView(channelPicker, new LinearLayout.LayoutParams(0, dp(48), 1));
        sustain = new Switch(this);
        sustain.setText("Sustain"); sustain.setTextColor(INK); sustain.setTextSize(13);
        sustain.setMinHeight(dp(48)); sustain.setPadding(dp(5), 0, 0, 0);
        playing.addView(sustain);
        performance.addView(playing);
        sustain.setOnCheckedChangeListener((button, checked) -> send(0xb0 | uiChannel, 64, checked ? 127 : 0));
        channelPicker.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                if (uiChannel != position) releaseNotes();
                uiChannel = position;
                pendingVoiceId = "";
                browser.setPendingId("");
                browser.setDestination(uiChannel);
                lastStateJson = "";
                disablePartControls();
                refreshCurrentSound();
            }
            @Override public void onNothingSelected(AdapterView<?> parent) {}
        });
        gainLabel = text("Volumen de salida", 12, INK);
        gainLabel.setSingleLine(true);
        gainLabel.setContentDescription("Volumen de salida");
        LinearLayout masterVolume = row();
        masterVolume.addView(gainLabel);
        gain = new SeekBar(this);
        gain.setContentDescription("Volumen de salida, porcentaje");
        gain.setMax(100); gain.setProgress(preferences.getInt("gain", 80));
        masterVolume.addView(gain, new LinearLayout.LayoutParams(0, dp(40), 1));
        performance.addView(masterVolume);
        gainLabel.setText("Volumen · " + gain.getProgress() + "%");
        gain.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar, int value, boolean fromUser) {
                gainLabel.setText("Volumen · " + value + "%");
                if (!fromUser) return;
                if (NativeEngine.LOAD_ERROR == null) NativeEngine.setGain(value / 100f);
                preferences.edit().putInt("gain", value).apply();
            }
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {}
        });
        toneGrid = new GridLayout(this);
        String[][] controls = {
                {"Corte", "cutoff", "74"}, {"Resonancia", "resonance", "71"},
                {"Ataque", "attack", "73"}, {"Release", "release", "72"},
                {"Reverb", "reverb", "91"}, {"Chorus", "chorus", "93"}
        };
        for (int i = 0; i < controls.length; i++) {
            String[] spec = controls[i];
            PartControlView control = new PartControlView(this, spec[0], spec[1],
                    Integer.parseInt(spec[2]), i < 4, (cc, value) -> send(0xb0 | uiChannel, cc, value));
            partControls.add(control); toneGrid.addView(control);
        }
        performance.addView(toneGrid);
        performance.addView(text("Corte, resonancia y tiempos: ajuste ±, centro 0. "
                + "Reverb/chorus: envío de las partes del canal. Mixto = valores distintos.", 10, MUTED));
        performanceScroll.addView(performance);
        instrumentBody.addView(performanceScroll);
        instrumentRoot.addView(instrumentBody, new LinearLayout.LayoutParams(-1, 0, 1));

        keyboardPanel = column();
        keyboardPanel.setPadding(0, dp(8), 0, 0);
        keyboard = new KeyboardView(this, new KeyboardView.Notes() {
            @Override public void on(int note) { send(0x90 | uiChannel, note, 100); }
            @Override public void off(int note) { send(0x80 | uiChannel, note, 0); }
        });
        keyboardPanel.addView(keyboard, new LinearLayout.LayoutParams(-1, dp(112)));
        keyboardPanel.setVisibility(preferences.getBoolean("keyboard_visible", false) ? View.VISIBLE : View.GONE);
        instrumentRoot.addView(keyboardPanel);
        updateKeyboardButton();

        buildConnections();
        buildDiagnostics();
        buildManualSelection();
        instrumentRoot.addOnLayoutChangeListener((v, l, t, r, b, oldL, oldT, oldR, oldB) -> {
            if (r - l != oldR - oldL || !layoutReady) reflowInstrument();
        });
        instrumentRoot.post(this::reflowInstrument);
        instrumentRoot.requestFocus();
    }

    private void reflowInstrument() {
        if (destroyed || instrumentRoot == null || browser == null || instrumentRoot.getWidth() == 0) return;
        int width = instrumentRoot.getWidth() - instrumentRoot.getPaddingLeft() - instrumentRoot.getPaddingRight();
        boolean wide = width >= dp(740);
        if (!layoutReady || wide != wideLayout) {
            wideLayout = wide; layoutReady = true;
            instrumentBody.setOrientation(wide ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
            LinearLayout.LayoutParams browserParams = wide
                    ? new LinearLayout.LayoutParams(0, -1, 1)
                    : new LinearLayout.LayoutParams(-1, 0, 1);
            LinearLayout.LayoutParams controlsParams = wide
                    ? new LinearLayout.LayoutParams(dp(250), -1)
                    : new LinearLayout.LayoutParams(-1, dp(246));
            if (wide) controlsParams.leftMargin = dp(8);
            else controlsParams.topMargin = dp(8);
            browser.setLayoutParams(browserParams);
            performanceScroll.setLayoutParams(controlsParams);
            keyboard.setLayoutParams(new LinearLayout.LayoutParams(-1, dp(wide ? 104 : 124)));
        }
        int columns = wide ? 2 : 3;
        if (columns != toneColumns) {
            toneColumns = columns;
            toneGrid.removeAllViews();
            toneGrid.setColumnCount(columns);
            for (int i = 0; i < partControls.size(); i++) {
                GridLayout.LayoutParams params = new GridLayout.LayoutParams(
                        GridLayout.spec(i / columns), GridLayout.spec(i % columns, 1f));
                params.width = 0; params.height = -2;
                toneGrid.addView(partControls.get(i), params);
            }
        }
    }

    @Override public void onConfigurationChanged(Configuration configuration) {
        super.onConfigurationChanged(configuration);
        if (keyboard != null) keyboard.releaseAll();
        if (instrumentRoot != null) instrumentRoot.post(this::reflowInstrument);
    }

    private void toggleKeyboard() {
        boolean show = keyboardPanel.getVisibility() != View.VISIBLE;
        if (!show) keyboard.releaseAll();
        keyboardPanel.setVisibility(show ? View.VISIBLE : View.GONE);
        preferences.edit().putBoolean("keyboard_visible", show).apply();
        updateKeyboardButton();
    }

    private void updateKeyboardButton() {
        keyboardToggle.setBackgroundTintList(ColorStateList.valueOf(
                keyboardPanel.getVisibility() == View.VISIBLE ? 0xff2278a6 : 0xff2c3b4e));
        keyboardToggle.setContentDescription(keyboardPanel.getVisibility() == View.VISIBLE
                ? "Ocultar teclado en pantalla" : "Mostrar teclado en pantalla");
    }

    private void showMenu(View anchor) {
        PopupMenu menu = new PopupMenu(this, anchor);
        menu.getMenu().add(0, 1, 0, "Conexiones · MIDI, audio y ROMs");
        menu.getMenu().add(0, 2, 1, "Diagnóstico");
        menu.getMenu().add(0, 3, 2, "Banco y programa manual");
        menu.getMenu().add(0, 4, 3, "Actualizar catálogo");
        menu.getMenu().add(0, 5, 4, "Acerca de / Licencias");
        menu.setOnMenuItemClickListener(item -> {
            switch (item.getItemId()) {
                case 1: showPanel(connectionsDialog, true); break;
                case 2: showPanel(diagnosticsDialog, true); break;
                case 3: showPanel(manualDialog, false); break;
                case 4: catalogAttemptKey = ""; requestCatalog(); break;
                case 5: showLicenses(); break;
                default: return false;
            }
            return true;
        });
        menu.show();
    }

    private void showPanel(AlertDialog dialog, boolean tall) {
        if (destroyed || !visible || dialog == null) return;
        dialog.show();
        if (dialog.getWindow() != null) {
            int width = Math.max(dp(280), instrumentRoot.getWidth() - dp(24));
            int height = Math.max(dp(240), instrumentRoot.getHeight() - dp(36));
            dialog.getWindow().setLayout(Math.min(dp(760), width),
                    tall ? height : WindowManager.LayoutParams.WRAP_CONTENT);
        }
    }

    private AlertDialog panelDialog(String title, LinearLayout contents) {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        contents.setPadding(dp(12), dp(4), dp(12), dp(12));
        scroll.addView(contents);
        return new AlertDialog.Builder(this).setTitle(title).setView(scroll)
                .setPositiveButton("Cerrar", null).create();
    }

    private void showSoundDetails() {
        if (NativeEngine.LOAD_ERROR != null) return;
        String[] sound = readCurrentSound();
        new AlertDialog.Builder(this).setTitle("Sonido confirmado por el MU2000")
                .setMessage(sound[0] + "\n\n" + sound[1]
                        + "\n\nLos controles y las cargas de la lista usan puerto A, canal " + (uiChannel + 1)
                        + ". Seguir Program Change sólo cambia lo que muestra esta pantalla.")
                .setPositiveButton("Cerrar", null).show();
    }

    private void buildConnections() {
        LinearLayout content = column();
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
        midiCard.addView(text("Se conservan los canales, sustain, pitch bend y SysEx del teclado. "
                + "El canal de los controles en pantalla no cambia el canal de tu teclado.", 12, MUTED));

        LinearLayout routing = card(content, CARD);
        heading(routing, "AUDIO");
        LinearLayout audioRow = row(); routing.addView(audioRow);
        LinearLayout outputCol = labelled(audioRow, "Salida", 3);
        outputPicker = spinner(); outputCol.addView(outputPicker);
        LinearLayout bufferCol = labelled(audioRow, "Buffer solicitado", 2);
        bufferPicker = spinner(); bufferCol.addView(bufferPicker);
        setItems(bufferPicker, new String[]{"512 frames", "1024 frames", "2048 frames"});
        int savedBuffer = preferences.getInt("buffer", 2048);
        bufferPicker.setSelection(savedBuffer == 512 ? 0 : savedBuffer == 2048 ? 2 : 1);
        routing.addView(text("CPU del motor", 11, MUTED));
        cpuPicker = spinner(); routing.addView(cpuPicker);
        cpuPicker.setPrompt("CPU del motor");
        setItems(cpuPicker, new String[]{"Automático (Android)", "Núcleos de rendimiento"});
        cpuPicker.setSelection(preferences.getInt("cpu_mode", 0) == 1 ? 1 : 0);
        routing.addView(text("Empezá con 2048; si va estable, probá 1024. Android elige por separado "
                + "el tamaño de cada bloque de audio (callback). Para cambiar salida, buffer o CPU, "
                + "detené el audio. Diagnóstico muestra los tamaños reales y los cortes.", 12, MUTED));

        LinearLayout romCard = card(content, CARD);
        heading(romCard, "ROMs DEL MU2000");
        romStatus = text("", 13, INK); romCard.addView(romStatus);
        LinearLayout romButtons = row(); romCard.addView(romButtons);
        importFiles = button("Importar archivos", v -> pick(false));
        importFolder = button("Importar carpeta", v -> pick(true));
        weighted(romButtons, importFiles, 1); weighted(romButtons, importFolder, 1);
        romCard.addView(text("Seleccioná mu2000_flash.bin (4 MiB) y las cuatro ROMs de ondas "
                + "(8 MiB cada una). Podés agregarlas en varias importaciones. También admite "
                + "sin-table.bin como archivo opcional. Los ZIP deben descomprimirse antes.", 12, MUTED));
        content.addView(text("La aplicación se detiene al salir y no se inicia sola al volver.", 11, MUTED));
        connectionsDialog = panelDialog("Conexiones", content);
    }

    private void buildDiagnostics() {
        LinearLayout content = column();
        diagnostics = text("Iniciá el audio para ver su estado.", 12, ACCENT);
        diagnostics.setTypeface(Typeface.MONOSPACE);
        diagnostics.setTextIsSelectable(true);
        content.addView(diagnostics);
        LinearLayout actions = row(); content.addView(actions);
        detailedDiagnostics = new Switch(this);
        detailedDiagnostics.setText("Detalle de cortes");
        detailedDiagnostics.setTextColor(INK); detailedDiagnostics.setTextSize(13);
        detailedDiagnostics.setMinHeight(dp(48)); detailedDiagnostics.setChecked(false);
        copyDiagnostics = button("Copiar diagnóstico", v -> copyDiagnostics());
        weighted(actions, detailedDiagnostics, 1); weighted(actions, copyDiagnostics, 1);
        content.addView(text("Activar detalle reinicia el registro; copialo antes si querés conservarlo.", 11, MUTED));
        cutDetails = column(); cutDetails.setVisibility(View.GONE); content.addView(cutDetails);
        cutDetails.addView(text("Se inicia un registro de los 8 bloques más lentos y los últimos 4 incrementos "
                + "de xruns, con datos de planificación. Copiar incluye los registros disponibles "
                + "aunque esta sección esté cerrada.", 11, MUTED));
        callbackTrace = text("Todavía no hay registros de audio.", 11, ACCENT);
        callbackTrace.setTypeface(Typeface.MONOSPACE); callbackTrace.setTextIsSelectable(true);
        cutDetails.addView(callbackTrace);
        detailedDiagnostics.setOnCheckedChangeListener((button, checked) -> {
            cutDetails.setVisibility(checked ? View.VISIBLE : View.GONE);
            if (NativeEngine.LOAD_ERROR == null) NativeEngine.setDetailedDiagnostics(checked);
            if (checked) refreshCallbackDiagnostics();
        });
        if (NativeEngine.LOAD_ERROR == null) NativeEngine.setDetailedDiagnostics(false);
        diagnosticsDialog = panelDialog("Diagnóstico de audio", content);
    }

    private void buildManualSelection() {
        LinearLayout content = column();
        content.addView(text("Valores que se enviarán al canal elegido en los controles del instrumento. "
                + "Programa visible: 1–128. El nombre confirmado aparece en la pantalla del MU2000.", 12, MUTED));
        LinearLayout fields = row(); content.addView(fields);
        bankMsb = number(labelled(fields, "Banco MSB", 1), preferences.getInt("msb", 0));
        bankLsb = number(labelled(fields, "Banco LSB", 1), preferences.getInt("lsb", 0));
        program = number(labelled(fields, "Programa", 1), preferences.getInt("program", 1));
        applyProgram = button("Enviar banco y programa", v -> applyVoice());
        content.addView(applyProgram);
        manualDialog = panelDialog("Selección manual · puerto A", content);
    }

    private void toolbarButton(LinearLayout row, Button button, int width) {
        button.setMinWidth(0); button.setMinimumWidth(0);
        button.setPadding(dp(4), dp(2), dp(4), dp(2));
        if (button.getText().length() > 1) button.setTextSize(12);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(dp(width), dp(48));
        params.leftMargin = dp(3); row.addView(button, params);
    }

    private GradientDrawable panelBackground(int color) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(color); drawable.setCornerRadius(dp(12));
        drawable.setStroke(dp(1), 0xff28384b); return drawable;
    }

    private void showLicenses() {
        worker.execute(() -> {
            StringBuilder contents = new StringBuilder("RiMU2000 Android · alpha " + APP_VERSION + "\n\n")
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
        final int cpuMode = cpuPicker.getSelectedItemPosition() == 1 ? 1 : 0;
        AudioChoice selected = selectedOutput();
        final int deviceId = selected == null ? 0 : selected.id;
        preferences.edit().putInt("buffer", frames).putInt("cpu_mode", cpuMode).apply();
        busy = true; starting = true;
        status.setText("Iniciando el MU2000… la primera carga puede tardar unos segundos.");
        soundName.setText("Iniciando el MU2000…");
        soundDetails.setText("El sonido se mostrará cuando el firmware esté listo.");
        updateControls();
        NativeEngine.setGain(gain.getProgress() / 100f);
        worker.execute(() -> {
            if (destroyed || !visible || request != generation) return;
            String result;
            try {
                result = NativeEngine.start(roms.getAbsolutePath(), state.getAbsolutePath(),
                        frames, deviceId, cpuMode);
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
                } else {
                    status.setText(error == null || error.isEmpty() ? "El motor no pudo iniciar." : error);
                    abandonFocus();
                }
                diagnostics.setText(NativeEngine.stats());
                refreshCurrentSound();
                refreshCallbackDiagnostics();
                updateControls();
            });
        });
    }

    private void requestStop(String message) {
        if (destroyed) return;
        releaseNotes();
        catalogLoading = false;
        pendingVoiceId = "";
        if (browser != null) browser.setPendingId("");
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
                refreshCurrentSound();
                refreshCallbackDiagnostics();
                refreshRoms();
                updateControls();
            });
        });
    }

    private void abandonFocus() { if (audio != null && focus != null) audio.abandonAudioFocusRequest(focus); }

    private String[] readCurrentSound() {
        String[] snapshot = NativeEngine.currentSound(uiChannel, followProgramChanges.isChecked());
        String names = snapshot != null && snapshot.length > 0 && snapshot[0] != null
                && !snapshot[0].isEmpty() ? snapshot[0] : "Nombre no disponible";
        String details = snapshot != null && snapshot.length > 1 && snapshot[1] != null
                ? snapshot[1] : "";
        String stateJson = snapshot != null && snapshot.length > 2 && snapshot[2] != null
                ? snapshot[2] : "";
        return new String[]{names, details, stateJson};
    }

    private void refreshCurrentSound() {
        if (destroyed || !visible || busy || soundName == null || followProgramChanges == null
                || NativeEngine.LOAD_ERROR != null) return;
        String[] snapshot = readCurrentSound();
        setTextIfChanged(soundName, snapshot[0]);
        setTextIfChanged(soundDetails, snapshot[1]);
        if (!snapshot[2].equals(lastStateJson)) {
            lastStateJson = snapshot[2];
            try {
                soundState = snapshot[2].isEmpty() ? null : new JSONObject(snapshot[2]);
            } catch (JSONException e) {
                soundState = null;
            }
        }
        // Reconcile gestures even if firmware kept the same state (for
        // example an ignored CC or selecting the already-current voice).
        applySoundState();
        requestCatalog();
        if (!pendingVoiceId.isEmpty() && SystemClock.uptimeMillis() - pendingVoiceAt > 3000) {
            status.setText("Solicitado: " + pendingVoiceName + ". El LCD muestra la respuesta real del MU2000.");
            pendingVoiceId = "";
            browser.setPendingId("");
        }
    }

    private void applySoundState() {
        snapshotCatalogKey = soundState == null ? "" : soundState.optString("catalogKey", "");
        boolean valid = soundState != null && soundState.optBoolean("running", false)
                && soundState.optBoolean("layoutKnown", false)
                && soundState.optInt("controlRoute", -1) == uiChannel;
        JSONArray parts = soundState == null ? null : soundState.optJSONArray("parts");
        valid &= parts != null && parts.length() > 0 && running && !busy;
        Set<String> current = new HashSet<>();
        for (PartControlView control : partControls) {
            boolean known = valid, mixed = false;
            int common = -1;
            if (valid) for (int i = 0; i < parts.length(); i++) {
                JSONObject part = parts.optJSONObject(i);
                int value = part == null ? -1 : part.optInt(control.field, -1);
                if (value < 0 || value > 127) { known = false; break; }
                if (i == 0) common = value;
                else if (common != value) mixed = true;
            }
            control.showValue(mixed ? -1 : common, known);
        }
        if (valid && catalog != null && catalog.key.equals(snapshotCatalogKey)) {
            for (int i = 0; i < parts.length(); i++) {
                JSONObject part = parts.optJSONObject(i);
                if (part == null) continue;
                int msb = part.optInt("msb", -1), lsb = part.optInt("lsb", -1);
                int program = part.optInt("program", -1), partMode = part.optInt("partMode", -1);
                for (PatchBrowserView.Entry entry : catalog.entries) {
                    if (entry.msb != msb || entry.lsb != lsb || entry.program != program) continue;
                    // A drum part with raw MSB 0 is ambiguous. The LCD still
                    // names its real kit, but the browser must not guess it.
                    boolean melodic = entry.kind == 0 && partMode == 0;
                    boolean drum = partMode > 0 && ((entry.kind == 1 && msb == 127)
                            || (entry.kind == 2 && msb == 126));
                    if (melodic || drum) current.add(entry.id);
                }
            }
        }
        browser.setCurrentIds(current);
        if (!pendingVoiceId.isEmpty() && current.contains(pendingVoiceId)) {
            pendingVoiceId = "";
            browser.setPendingId("");
            status.setText("Banco y programa recibidos · puerto A, canal " + (uiChannel + 1));
        }
        updateControls();
    }

    private void disablePartControls() {
        for (PartControlView control : partControls) control.showValue(-1, false);
    }

    private void requestCatalog() {
        if (destroyed || !visible || busy || !running || catalogLoading
                || NativeEngine.LOAD_ERROR != null || snapshotCatalogKey.isEmpty()
                || snapshotCatalogKey.equals(catalogAttemptKey)) return;
        final String wanted = snapshotCatalogKey;
        final int request = generation;
        catalogAttemptKey = wanted;
        catalogLoading = true;
        browser.setCanLoad(false);
        browser.setCatalog("LEYENDO SONIDOS…", Collections.emptyList(), "Leyendo los nombres y bancos de la ROM…");
        worker.execute(() -> {
            CatalogData parsed = null;
            String failure = null;
            try {
                parsed = new CatalogData(NativeEngine.voiceCatalog());
            } catch (RuntimeException | LinkageError | JSONException e) {
                failure = "No se pudo leer el catálogo. Usá Actualizar catálogo para volver a intentar.";
            }
            final CatalogData result = parsed;
            final String error = failure;
            main.post(() -> {
                if (destroyed || request != generation) return;
                catalogLoading = false;
                if (error != null || result == null) {
                    catalog = null;
                    browser.setCatalog("SONIDOS", Collections.emptyList(), error);
                } else {
                    catalog = result;
                    browser.setCatalog(result.title(), result.entries, result.status);
                    applySoundState();
                }
                updateControls();
            });
        });
    }

    private void loadPatch(PatchBrowserView.Entry entry) {
        if (!running || busy || catalog == null || !catalog.recognized
                || !catalog.key.equals(snapshotCatalogKey) || NativeEngine.LOAD_ERROR != null) return;
        keyboard.releaseAll();
        String error = NativeEngine.selectVoice(catalog.key, entry.msb, entry.lsb, entry.program,
                entry.kind, uiChannel);
        if (error == null || !error.isEmpty()) {
            status.setText(error == null ? "No se pudo solicitar ese sonido." : error);
            return;
        }
        pendingVoiceId = entry.id;
        pendingVoiceName = entry.name;
        pendingVoiceAt = SystemClock.uptimeMillis();
        browser.setPendingId(entry.id);
        status.setText("Solicitado: " + entry.name + " · A, canal " + (uiChannel + 1) + ". Esperando al MU2000…");
        // Keep the manual proposal useful, without copying firmware changes
        // into it or sending additional MIDI through text listeners.
        bankMsb.setText(Integer.toString(entry.msb));
        bankLsb.setText(Integer.toString(entry.lsb));
        program.setText(Integer.toString(entry.program + 1));
        preferences.edit().putInt("msb", entry.msb).putInt("lsb", entry.lsb)
                .putInt("program", entry.program + 1).apply();
    }

    private static final class CatalogData {
        final String key, status;
        final int mode, voiceSet;
        final boolean recognized;
        final List<PatchBrowserView.Entry> entries = new ArrayList<>();
        CatalogData(String json) throws JSONException {
            JSONObject object = new JSONObject(json);
            key = object.optString("key", "");
            status = object.optString("status", "No hay catálogo disponible.");
            mode = object.optInt("mode", -1);
            voiceSet = object.optInt("voiceSet", -1);
            recognized = object.optBoolean("recognized", false);
            JSONArray voices = object.optJSONArray("voices");
            if (!recognized || voices == null) return;
            for (int i = 0; i < voices.length(); i++) {
                JSONObject voice = voices.optJSONObject(i);
                if (voice == null) continue;
                String id = voice.optString("id", ""), name = voice.optString("name", "");
                int msb = voice.optInt("msb", -1), lsb = voice.optInt("lsb", -1);
                int program = voice.optInt("program", -1), kind = voice.optInt("kind", -1);
                int family = voice.optInt("family", -1);
                if (id.isEmpty() || name.isEmpty() || msb < 0 || msb > 127 || lsb < 0 || lsb > 127
                        || program < 0 || program > 127 || kind < 0 || kind > 2) continue;
                String category = family >= 0 && family < FAMILIES.length ? FAMILIES[family] : "Otros";
                entries.add(new PatchBrowserView.Entry(id, name, category, msb, lsb, program, kind));
            }
        }
        String title() {
            return mode == 1 ? "SONIDOS · XG" : mode == 0 ? "SONIDOS · GM · Melódicos"
                    : "SONIDOS · Modo " + mode;
        }
    }

    private void refreshCallbackDiagnostics() {
        if (destroyed || callbackTrace == null || detailedDiagnostics == null
                || !detailedDiagnostics.isChecked() || NativeEngine.LOAD_ERROR != null) return;
        String trace = NativeEngine.callbackDiagnostics();
        setTextIfChanged(callbackTrace, trace == null || trace.isEmpty()
                ? "Todavía no hay registros de audio." : trace);
    }

    private void copyDiagnostics() {
        if (destroyed || !visible || NativeEngine.LOAD_ERROR != null) return;
        String[] sound = readCurrentSound();
        String stats = NativeEngine.stats();
        String trace = NativeEngine.callbackDiagnostics();
        String report = "RiMU2000 Android · alpha " + APP_VERSION
                + "\n\nSONIDO ACTUAL\n" + sound[0] + "\n" + sound[1]
                + "\nSeguimiento de Program Change: " + (followProgramChanges.isChecked() ? "sí" : "no")
                + "\nControles en pantalla: puerto A, canal " + (uiChannel + 1)
                + "\n\nESTADO DEL AUDIO\n" + (stats == null ? "No disponible" : stats)
                + "\n\nBLOQUES DE AUDIO REGISTRADOS\n" + (trace == null ? "No disponibles" : trace)
                + "\n\nENTRADA MIDI\n" + midiStatus.getText();
        ClipboardManager clipboard = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
        if (clipboard == null) {
            Toast.makeText(this, "No se pudo acceder al portapapeles.", Toast.LENGTH_SHORT).show();
            return;
        }
        clipboard.setPrimaryClip(ClipData.newPlainText("Diagnóstico RiMU2000", report));
        // Android 13+ provides its own confirmation for clipboard writes.
        if (Build.VERSION.SDK_INT < 33)
            Toast.makeText(this, "Diagnóstico copiado.", Toast.LENGTH_SHORT).show();
    }

    private void setTextIfChanged(TextView view, String value) {
        if (!value.contentEquals(view.getText())) view.setText(value);
    }

    private void releaseNotes() {
        if (keyboard != null) keyboard.releaseAll();
        if (sustain != null && sustain.isChecked()) sustain.setChecked(false);
    }

    private void applyVoice() {
        if (!running) return;
        Integer msb = bounded(bankMsb, 0, 127), lsb = bounded(bankLsb, 0, 127);
        Integer pc = bounded(program, 1, 128);
        if (msb == null || lsb == null || pc == null) return;
        keyboard.releaseAll();
        byte[] bytes = {(byte)(0xb0 | uiChannel), 0, msb.byteValue(),
                (byte)(0xb0 | uiChannel), 32, lsb.byteValue(),
                (byte)(0xc0 | uiChannel), (byte)(pc - 1)};
        NativeEngine.sendMidi(bytes, 0, bytes.length, 0);
        pendingVoiceId = "";
        browser.setPendingId("");
        status.setText("Enviado banco " + msb + "/" + lsb + " · programa " + pc
                + " · A, canal " + (uiChannel + 1) + ". El LCD confirma el sonido real.");
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
        cpuPicker.setEnabled(!busy && !running);
        importFiles.setEnabled(!busy && !running);
        importFolder.setEnabled(!busy && !running);
        applyProgram.setEnabled(available && running);
        keyboard.setEnabled(available && running);
        sustain.setEnabled(available && running);
        gain.setEnabled(available);
        followProgramChanges.setEnabled(available);
        detailedDiagnostics.setEnabled(available);
        copyDiagnostics.setEnabled(available);
        browser.setCanLoad(available && running && !busy && catalog != null && catalog.recognized
                && catalog.key.equals(snapshotCatalogKey));
        if (!running || busy) disablePartControls();
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
