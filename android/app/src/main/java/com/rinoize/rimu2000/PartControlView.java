package com.rinoize.rimu2000;

import android.content.Context;
import android.graphics.Typeface;
import android.text.TextUtils;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

/** One MIDI part control. Firmware refreshes never call the gesture listener. */
final class PartControlView extends LinearLayout {
    interface Gesture { void change(int controller, int value); }
    final String field;
    private final int controller;
    private final boolean centered;
    private final Gesture gesture;
    private final SeekBar slider;
    private final TextView value;
    private boolean available, dragging;

    PartControlView(Context context, String label, String field, int controller,
                    boolean centered, Gesture gesture) {
        super(context);
        this.field = field;
        this.controller = controller;
        this.centered = centered;
        this.gesture = gesture;
        setOrientation(VERTICAL);
        setPadding(dp(3), dp(4), dp(3), dp(2));
        LinearLayout heading = new LinearLayout(context);
        heading.setGravity(Gravity.CENTER_VERTICAL);
        TextView name = new TextView(context);
        name.setText(label); name.setTextSize(12); name.setTextColor(0xffc7d4e3);
        name.setSingleLine(true); name.setEllipsize(TextUtils.TruncateAt.END);
        heading.addView(name, new LayoutParams(0, -2, 1));
        value = new TextView(context);
        value.setText("—"); value.setTextSize(12); value.setTextColor(0xff7fd5ff);
        value.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        heading.addView(value);
        addView(heading);
        slider = new SeekBar(context);
        slider.setMax(127);
        slider.setProgress(centered ? 64 : 0);
        slider.setEnabled(false);
        slider.setContentDescription(label);
        addView(slider, new LayoutParams(-1, dp(40)));
        slider.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar, int progress, boolean fromUser) {
                if (!fromUser || !available) return;
                value.setText(format(progress));
                PartControlView.this.gesture.change(PartControlView.this.controller, progress);
            }
            @Override public void onStartTrackingTouch(SeekBar bar) { dragging = true; }
            @Override public void onStopTrackingTouch(SeekBar bar) { dragging = false; }
        });
    }

    /** commonValue=-1 means mixed; usable=false means no authoritative value. */
    void showValue(int commonValue, boolean usable) {
        available = usable;
        slider.setEnabled(usable);
        if (dragging && usable) return;
        if (!usable) {
            value.setText("—");
        } else if (commonValue < 0) {
            value.setText("Mixto");
            slider.setProgress(centered ? 64 : 0);
        } else {
            slider.setProgress(commonValue);
            value.setText(format(commonValue));
        }
    }

    private String format(int raw) {
        if (!centered) return Integer.toString(raw);
        int offset = raw - 64;
        return offset > 0 ? "+" + offset : Integer.toString(offset);
    }
    @Override protected void onDetachedFromWindow() {
        dragging = false;
        super.onDetachedFromWindow();
    }
    private int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }
}
