package com.rinoize.rimu2000;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.text.Editable;
import android.text.TextWatcher;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.ToggleButton;

import java.text.Normalizer;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/** A local, recycled browser. Filtering and stars never send MIDI. */
final class PatchBrowserView extends LinearLayout {
    interface Listener { void load(Entry entry); }

    /** MIDI values are zero based; the stable identity is supplied by the catalog. */
    static final class Entry {
        final String id, name, category;
        final int msb, lsb, program, kind;
        final String bank, numbers, search;

        Entry(String id, String name, String category, int msb, int lsb, int program, int kind) {
            this.id = id;
            this.name = name;
            this.category = category;
            this.msb = msb;
            this.lsb = lsb;
            this.program = program;
            this.kind = kind;
            bank = msb + " / " + lsb;
            numbers = String.format(Locale.ROOT, "P %03d  ·  Banco %03d / %03d", program + 1, msb, lsb);
            search = normalize(name + " " + category + " " + numbers + " "
                    + (program + 1) + " " + msb + "/" + lsb + " " + msb + " " + lsb);
        }
    }

    private static final int INK = 0xffe6edf5, MUTED = 0xffa0afc1, ACCENT = 0xff7fd5ff;
    private final SharedPreferences preferences;
    private final Listener listener;
    private final Set<String> favorites;
    private Set<String> currentIds = Collections.emptySet();
    private String pendingId = "", catalogTitle = "SONIDOS", unavailable = "Iniciá el audio para leer los sonidos de la ROM.";
    private boolean canLoad, replacingFilters;
    private final List<Entry> entries = new ArrayList<>(), filtered = new ArrayList<>();
    private final List<String> categories = new ArrayList<>(), banks = new ArrayList<>();
    private final TextView title, destination, count, empty;
    private final EditText search;
    private final Spinner category, bank;
    private final ToggleButton onlyFavorites;
    private final ListView list;
    private final PatchAdapter adapter = new PatchAdapter();

    PatchBrowserView(Context context, SharedPreferences preferences, Listener listener) {
        super(context);
        this.preferences = preferences;
        this.listener = listener;
        favorites = new HashSet<>(preferences.getStringSet("patch_favorites", Collections.emptySet()));
        setOrientation(VERTICAL);
        setPadding(dp(12), dp(10), dp(12), dp(8));
        setBackground(background(0xff151f2c));

        LinearLayout heading = row();
        title = label("SONIDOS", 12, ACCENT);
        title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        heading.addView(title, new LayoutParams(0, -2, 1));
        count = label("", 11, MUTED);
        heading.addView(count);
        addView(heading);
        destination = label("Cargar en puerto A · canal 1", 11, MUTED);
        destination.setPadding(0, dp(4), 0, dp(7));
        addView(destination);

        LinearLayout searchRow = row();
        search = new EditText(context);
        search.setSingleLine(true);
        search.setTextColor(INK);
        search.setHintTextColor(MUTED);
        search.setTextSize(15);
        search.setHint("Buscar nombre, banco o programa");
        search.setContentDescription("Buscar sonidos por nombre, banco o programa");
        search.setImeOptions(EditorInfo.IME_ACTION_DONE);
        search.setInputType(android.text.InputType.TYPE_CLASS_TEXT
                | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        searchRow.addView(search, new LayoutParams(0, dp(44), 1));
        Button clear = smallButton("×");
        clear.setTextSize(23);
        clear.setContentDescription("Limpiar búsqueda");
        clear.setOnClickListener(v -> search.setText(""));
        searchRow.addView(clear, new LayoutParams(dp(44), dp(44)));
        addView(searchRow);

        LinearLayout filters = row();
        category = new Spinner(context, Spinner.MODE_DIALOG);
        category.setPrompt("Categoría de sonidos");
        category.setContentDescription("Filtrar por categoría");
        bank = new Spinner(context, Spinner.MODE_DIALOG);
        bank.setPrompt("Banco MSB / LSB");
        bank.setContentDescription("Filtrar por banco MSB y LSB");
        filters.addView(category, new LayoutParams(0, dp(48), 1));
        filters.addView(bank, new LayoutParams(0, dp(48), 1));
        onlyFavorites = new ToggleButton(context);
        onlyFavorites.setTextOff("☆ Favoritos");
        onlyFavorites.setTextOn("★ Favoritos");
        onlyFavorites.setTextColor(INK);
        onlyFavorites.setTextSize(12);
        onlyFavorites.setAllCaps(false);
        onlyFavorites.setChecked(false);
        onlyFavorites.setPadding(dp(4), 0, dp(4), 0);
        onlyFavorites.setBackgroundTintList(ColorStateList.valueOf(0xff2c3b4e));
        filters.addView(onlyFavorites, new LayoutParams(dp(108), dp(48)));
        addView(filters);

        FrameLayout results = new FrameLayout(context);
        list = new ListView(context);
        list.setDivider(null);
        list.setDividerHeight(dp(4));
        list.setClipToPadding(false);
        list.setPadding(0, dp(4), 0, dp(4));
        list.setAdapter(adapter);
        list.setOnItemClickListener((parent, view, position, id) -> {
            if (canLoad && position >= 0 && position < filtered.size()) {
                hideSearchKeyboard();
                listener.load(filtered.get(position));
            }
        });
        results.addView(list, new FrameLayout.LayoutParams(-1, -1));
        empty = label(unavailable, 15, MUTED);
        empty.setGravity(Gravity.CENTER);
        empty.setPadding(dp(18), dp(16), dp(18), dp(16));
        results.addView(empty, new FrameLayout.LayoutParams(-1, -1));
        list.setEmptyView(empty);
        addView(results, new LayoutParams(-1, 0, 1));

        AdapterView.OnItemSelectedListener selected = new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                if (!replacingFilters) filter();
            }
            @Override public void onNothingSelected(AdapterView<?> parent) {}
        };
        category.setOnItemSelectedListener(selected);
        bank.setOnItemSelectedListener(selected);
        onlyFavorites.setOnCheckedChangeListener((button, checked) -> filter());
        search.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) { filter(); }
            @Override public void afterTextChanged(Editable text) {}
        });
        search.setOnEditorActionListener((view, action, event) -> {
            if (action != EditorInfo.IME_ACTION_DONE) return false;
            hideSearchKeyboard();
            return true;
        });
        replaceFilters();
    }

    void setCatalog(String title, List<Entry> values, String message) {
        catalogTitle = title;
        unavailable = message == null || message.isEmpty()
                ? "No hay sonidos disponibles en este catálogo." : message;
        entries.clear();
        entries.addAll(values);
        replaceFilters();
    }

    void setDestination(int channel) {
        destination.setText("Tocá un sonido para cargarlo en puerto A · canal " + (channel + 1));
    }

    void setCanLoad(boolean enabled) {
        if (canLoad == enabled) return;
        canLoad = enabled;
        adapter.notifyDataSetChanged();
    }

    void setCurrentIds(Set<String> ids) {
        if (currentIds.equals(ids)) return;
        currentIds = new HashSet<>(ids);
        adapter.notifyDataSetChanged();
    }

    void setPendingId(String id) {
        String next = id == null ? "" : id;
        if (pendingId.equals(next)) return;
        pendingId = next;
        adapter.notifyDataSetChanged();
    }

    private void replaceFilters() {
        String previousCategory = selected(category, categories);
        String previousBank = selected(bank, banks);
        LinkedHashSet<String> foundCategories = new LinkedHashSet<>();
        List<String> foundBanks = new ArrayList<>();
        Set<String> seenBanks = new HashSet<>();
        for (Entry entry : entries) {
            foundCategories.add(entry.category);
            if (seenBanks.add(entry.bank)) foundBanks.add(entry.bank);
        }
        // Order banks numerically, not as strings (2 precedes 10).
        foundBanks.sort((a, b) -> {
            String[] aa = a.split(" / "), bb = b.split(" / ");
            int cmp = Integer.compare(Integer.parseInt(aa[0]), Integer.parseInt(bb[0]));
            return cmp != 0 ? cmp : Integer.compare(Integer.parseInt(aa[1]), Integer.parseInt(bb[1]));
        });
        replacingFilters = true;
        categories.clear(); categories.add("Todas las categorías"); categories.addAll(foundCategories);
        banks.clear(); banks.add("Todos los bancos"); banks.addAll(foundBanks);
        category.setAdapter(spinnerAdapter(categories));
        bank.setAdapter(spinnerAdapter(banks));
        category.setSelection(Math.max(0, categories.indexOf(previousCategory)));
        bank.setSelection(Math.max(0, banks.indexOf(previousBank)));
        replacingFilters = false;
        filter();
    }

    private void filter() {
        String chosenCategory = selected(category, categories);
        String chosenBank = selected(bank, banks);
        String query = normalize(search.getText().toString().trim());
        String[] words = query.isEmpty() ? new String[0] : query.split("\\s+");
        filtered.clear();
        for (Entry entry : entries) {
            if (onlyFavorites.isChecked() && !favorites.contains(entry.id)) continue;
            if (category.getSelectedItemPosition() > 0 && !entry.category.equals(chosenCategory)) continue;
            if (bank.getSelectedItemPosition() > 0 && !entry.bank.equals(chosenBank)) continue;
            boolean matches = true;
            for (String word : words) if (!entry.search.contains(word)) { matches = false; break; }
            if (matches) filtered.add(entry);
        }
        title.setText(catalogTitle);
        count.setText(entries.isEmpty() ? "" : filtered.size() + " / " + entries.size());
        empty.setText(entries.isEmpty() ? unavailable : onlyFavorites.isChecked()
                ? "No hay favoritos con estos filtros. Marcá un sonido con ☆ para guardarlo."
                : "No hay coincidencias. Probá otro nombre, categoría o banco.");
        adapter.notifyDataSetChanged();
    }

    private void star(Entry entry) {
        if (!favorites.add(entry.id)) favorites.remove(entry.id);
        preferences.edit().putStringSet("patch_favorites", new HashSet<>(favorites)).apply();
        filter();
    }

    private void hideSearchKeyboard() {
        search.clearFocus();
        InputMethodManager input = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        if (input != null) input.hideSoftInputFromWindow(search.getWindowToken(), 0);
    }

    private final class PatchAdapter extends BaseAdapter {
        @Override public int getCount() { return filtered.size(); }
        @Override public Entry getItem(int position) { return filtered.get(position); }
        @Override public long getItemId(int position) { return position; }
        @Override public View getView(int position, View convertView, ViewGroup parent) {
            Row holder;
            if (convertView == null) {
                holder = new Row();
                convertView = holder.root;
                convertView.setTag(holder);
            } else holder = (Row) convertView.getTag();
            Entry entry = getItem(position);
            boolean current = currentIds.contains(entry.id), pending = pendingId.equals(entry.id);
            holder.name.setText(entry.name);
            holder.name.setTextColor(canLoad ? INK : MUTED);
            holder.details.setText(entry.numbers + (current ? "  ·  BANCO ACTUAL" : pending ? "  ·  SOLICITADO" : ""));
            holder.details.setTextColor(current || pending ? ACCENT : MUTED);
            holder.background.setColor(current ? 0xff143b51 : pending ? 0xff21354a : 0xff1a2737);
            holder.star.setText(favorites.contains(entry.id) ? "★" : "☆");
            holder.star.setContentDescription((favorites.contains(entry.id) ? "Quitar de favoritos: "
                    : "Guardar en favoritos: ") + entry.name);
            holder.star.setOnClickListener(v -> star(entry));
            holder.root.setContentDescription(entry.name + ", programa " + (entry.program + 1)
                    + ", banco " + entry.msb + ", " + entry.lsb + (current ? ", banco y programa actuales" : ""));
            return convertView;
        }
    }

    private final class Row {
        final LinearLayout root = row();
        final TextView name = label("", 18, INK), details = label("", 11, MUTED);
        final Button star = smallButton("☆");
        final GradientDrawable background = background(0xff1a2737);
        Row() {
            root.setPadding(dp(11), dp(5), dp(3), dp(5));
            root.setMinimumHeight(dp(56));
            root.setBackground(background);
            root.setDescendantFocusability(ViewGroup.FOCUS_BLOCK_DESCENDANTS);
            LinearLayout words = new LinearLayout(getContext());
            words.setOrientation(VERTICAL);
            name.setSingleLine(true); name.setEllipsize(TextUtils.TruncateAt.END);
            details.setSingleLine(true); details.setEllipsize(TextUtils.TruncateAt.END);
            details.setPadding(0, dp(3), 0, 0);
            words.addView(name); words.addView(details);
            root.addView(words, new LayoutParams(0, -2, 1));
            star.setTextSize(23);
            star.setTextColor(ACCENT);
            star.setFocusable(false);
            root.addView(star, new LayoutParams(dp(48), dp(48)));
        }
    }

    private ArrayAdapter<String> spinnerAdapter(List<String> values) {
        ArrayAdapter<String> result = new ArrayAdapter<String>(getContext(), android.R.layout.simple_spinner_item, values) {
            @Override public View getView(int position, View convertView, ViewGroup parent) {
                TextView view = (TextView) super.getView(position, convertView, parent);
                view.setTextColor(INK); view.setTextSize(12); view.setSingleLine(true);
                view.setEllipsize(TextUtils.TruncateAt.END);
                return view;
            }
        };
        result.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        return result;
    }

    private static String selected(Spinner spinner, List<String> values) {
        int index = spinner.getSelectedItemPosition();
        return index >= 0 && index < values.size() ? values.get(index) : "";
    }
    private static String normalize(String value) {
        return Normalizer.normalize(value, Normalizer.Form.NFD).replaceAll("\\p{M}+", "")
                .toLowerCase(Locale.ROOT);
    }
    private LinearLayout row() {
        LinearLayout result = new LinearLayout(getContext());
        result.setOrientation(HORIZONTAL); result.setGravity(Gravity.CENTER_VERTICAL); return result;
    }
    private TextView label(String value, int size, int color) {
        TextView result = new TextView(getContext());
        result.setText(value); result.setTextSize(size); result.setTextColor(color); return result;
    }
    private Button smallButton(String value) {
        Button result = new Button(getContext());
        result.setText(value); result.setTextColor(INK); result.setAllCaps(false);
        result.setMinWidth(0); result.setMinimumWidth(0);
        result.setPadding(0, 0, 0, 0);
        result.setBackgroundTintList(ColorStateList.valueOf(0xff26384c)); return result;
    }
    private GradientDrawable background(int color) {
        GradientDrawable result = new GradientDrawable(); result.setColor(color);
        result.setCornerRadius(dp(10)); return result;
    }
    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
}
