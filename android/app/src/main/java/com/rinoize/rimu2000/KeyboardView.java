package com.rinoize.rimu2000;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.util.SparseIntArray;
import android.view.MotionEvent;
import android.view.View;

/** Two octaves with real pointer tracking: every cancellation releases its notes. */
final class KeyboardView extends View {
    interface Notes { void on(int note); void off(int note); }
    private static final int[] WHITES = {48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71};
    private static final int[] BLACKS = {49, 51, 54, 56, 58, 61, 63, 66, 68, 70};
    private static final int[] BLACK_AFTER = {0, 1, 3, 4, 5, 7, 8, 10, 11, 12};
    private final SparseIntArray pointers = new SparseIntArray();
    private final int[] held = new int[128];
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Notes notes;
    private final float density;

    KeyboardView(Context context, Notes notes) {
        super(context);
        this.notes = notes;
        density = getResources().getDisplayMetrics().density;
        setContentDescription("Teclado de dos octavas, Do 3 a Si 4");
        setFocusable(true);
    }

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        float width = getWidth() / (float) WHITES.length;
        for (int i = 0; i < WHITES.length; i++) {
            int note = WHITES[i];
            paint.setColor(held[note] > 0 ? 0xff7fd3ff : (isEnabled() ? 0xffedf1f5 : 0xff858d97));
            canvas.drawRoundRect(i * width + 1, 0, (i + 1) * width - 1,
                    getHeight() - 1, 4 * density, 4 * density, paint);
            if (note % 12 == 0) {
                paint.setColor(0xff344355);
                paint.setTextSize(11 * density);
                paint.setTextAlign(Paint.Align.CENTER);
                canvas.drawText("C" + (note / 12 - 1), (i + .5f) * width,
                        getHeight() - 10 * density, paint);
            }
        }
        for (int i = 0; i < BLACKS.length; i++) {
            paint.setColor(held[BLACKS[i]] > 0 ? 0xff268ec4 : 0xff111923);
            float centre = (BLACK_AFTER[i] + 1) * width;
            canvas.drawRoundRect(centre - width * .30f, 0,
                    centre + width * .30f, getHeight() * .62f,
                    3 * density, 3 * density, paint);
        }
    }

    private int hit(float x, float y) {
        if (x < 0 || x >= getWidth() || y < 0 || y >= getHeight()) return -1;
        float width = getWidth() / (float) WHITES.length;
        if (y < getHeight() * .62f) {
            for (int i = 0; i < BLACKS.length; i++) {
                float centre = (BLACK_AFTER[i] + 1) * width;
                if (Math.abs(x - centre) < width * .30f) return BLACKS[i];
            }
        }
        return WHITES[Math.min(WHITES.length - 1, (int)(x / width))];
    }

    private void movePointer(int id, int note) {
        int old = pointers.get(id, -1);
        if (old == note) return;
        if (old >= 0 && --held[old] == 0) notes.off(old);
        if (note < 0) pointers.delete(id);
        else {
            pointers.put(id, note);
            if (held[note]++ == 0) notes.on(note);
        }
        invalidate();
    }

    @Override public boolean onTouchEvent(MotionEvent event) {
        if (!isEnabled()) return false;
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            getParent().requestDisallowInterceptTouchEvent(true);
            int index = event.getActionIndex();
            movePointer(event.getPointerId(index), hit(event.getX(index), event.getY(index)));
            return true;
        }
        if (action == MotionEvent.ACTION_MOVE) {
            for (int i = 0; i < event.getPointerCount(); i++) {
                movePointer(event.getPointerId(i), hit(event.getX(i), event.getY(i)));
            }
            return true;
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            movePointer(event.getPointerId(event.getActionIndex()), -1);
            if (action == MotionEvent.ACTION_UP) {
                getParent().requestDisallowInterceptTouchEvent(false);
                performClick();
            }
            return true;
        }
        if (action == MotionEvent.ACTION_CANCEL) {
            releaseAll();
            getParent().requestDisallowInterceptTouchEvent(false);
            return true;
        }
        return super.onTouchEvent(event);
    }

    void releaseAll() {
        pointers.clear();
        for (int note = 0; note < held.length; note++) {
            if (held[note] > 0) {
                held[note] = 0;
                notes.off(note);
            }
        }
        invalidate();
    }

    @Override protected void onDetachedFromWindow() {
        releaseAll();
        super.onDetachedFromWindow();
    }

    @Override public boolean performClick() { super.performClick(); return true; }
}
