package com.paopao.switchkey;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.util.SparseArray;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;
import java.util.List;

/**
 * 纯 Canvas 绘制的屏幕键盘，支持多指同时按（真键盘的 Shift+A 那种用法）。
 * 按键状态的判定逻辑在 {@link KeyState} 里，这里只负责布局、绘制和触摸分发。
 */
public class KeyboardView extends View {

    public interface ReportSink {
        void onReport(int modifiers, int[] usages, int count);
    }

    private static final int UNITS = 12;

    private static final int COLOR_BG = 0xFF14161A;
    private static final int COLOR_KEY = 0xFF2B2F37;
    private static final int COLOR_KEY_SPECIAL = 0xFF20242B;
    private static final int COLOR_KEY_MOD = 0xFF3A4049;
    private static final int COLOR_KEY_DOWN = 0xFF4C9AFF;
    private static final int COLOR_TEXT = 0xFFE9ECF1;
    private static final int COLOR_TEXT_DIM = 0xFF8C949F;

    private final KeyState state = new KeyState();

    private List<KeyboardLayout.Key[]> rows = new ArrayList<>();
    private final List<KeyboardLayout.Key> keys = new ArrayList<>();
    private final List<RectF> rects = new ArrayList<>();
    private final SparseArray<KeyboardLayout.Key> pointerKeys = new SparseArray<>();

    private final Paint bgPaint = new Paint();
    private final Paint keyPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint labelPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint subPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private float unit;
    private float rowHeight;
    private float pad;
    private float labelSize;

    public KeyboardView(Context ctx) {
        super(ctx);
        setFocusable(true);
        setClickable(true);
        bgPaint.setColor(COLOR_BG);
        labelPaint.setColor(COLOR_TEXT);
        labelPaint.setTextAlign(Paint.Align.CENTER);
        labelPaint.setTypeface(Typeface.DEFAULT_BOLD);
        subPaint.setColor(COLOR_TEXT_DIM);
        subPaint.setTextAlign(Paint.Align.RIGHT);
        subPaint.setTypeface(Typeface.DEFAULT);
        setLayout(KeyboardLayout.mainRows());
    }

    public void setLayout(List<KeyboardLayout.Key[]> newRows) {
        rows = newRows;
        keys.clear();
        for (KeyboardLayout.Key[] row : rows) {
            for (KeyboardLayout.Key k : row) {
                keys.add(k);
            }
        }
        rects.clear();
        if (getWidth() > 0 && getHeight() > 0) {
            computeMetrics(getWidth(), getHeight());
            recomputeRects(getWidth());
        }
        invalidate();
    }

    public void setSink(final ReportSink s) {
        if (s == null) {
            state.setSink(null);
            return;
        }
        state.setSink(new KeyState.Sink() {
            @Override
            public void onReport(int modifiers, int[] usages, int count) {
                s.onReport(modifiers, usages, count);
            }
        });
    }

    /** 松开所有按键，避免切走时按键卡住 */
    public void releaseAll() {
        pointerKeys.clear();
        state.releaseAll();
        invalidate();
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        if (w <= 0 || h <= 0 || keys.isEmpty()) {
            return;
        }
        computeMetrics(w, h);
        recomputeRects(w);
    }

    private void computeMetrics(int w, int h) {
        unit = (float) w / UNITS;
        rowHeight = (float) h / rows.size();
        pad = Math.max(2f, unit * 0.05f);
        labelSize = Math.min(rowHeight * 0.34f, unit * 0.52f);
    }

    private void recomputeRects(int w) {
        rects.clear();
        for (int r = 0; r < rows.size(); r++) {
            KeyboardLayout.Key[] row = rows.get(r);
            float total = 0f;
            for (KeyboardLayout.Key k : row) {
                total += k.width;
            }
            float x = (w - total * unit) / 2f;
            float y = r * rowHeight;
            for (KeyboardLayout.Key k : row) {
                float kw = k.width * unit;
                rects.add(new RectF(x + pad, y + pad, x + kw - pad, y + rowHeight - pad));
                x += kw;
            }
        }
    }

    // ---------------- 触摸 ----------------

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int idx = e.getActionIndex();
                int id = e.getPointerId(idx);
                KeyboardLayout.Key k = keyAt(e.getX(idx), e.getY(idx));
                if (k != null && pointerKeys.indexOfValue(k) < 0) {
                    pointerKeys.put(id, k);
                    performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
                    state.keyDown(k.usage, k.mod);
                    invalidate();
                }
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                // 手指滑出按键后仍归属原键（和手机输入法一致）
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int idx = e.getActionIndex();
                int id = e.getPointerId(idx);
                KeyboardLayout.Key k = pointerKeys.get(id);
                if (k != null) {
                    pointerKeys.remove(id);
                    state.keyUp(k.usage, k.mod);
                    invalidate();
                }
                return true;
            }
            case MotionEvent.ACTION_CANCEL: {
                releaseAll();
                return true;
            }
            default:
                return super.onTouchEvent(e);
        }
    }

    private KeyboardLayout.Key keyAt(float x, float y) {
        for (int i = 0; i < rects.size(); i++) {
            RectF r = rects.get(i);
            if (r.contains(x, y)) {
                return keys.get(i);
            }
        }
        return null;
    }

    // ---------------- 绘制 ----------------

    @Override
    protected void onDraw(Canvas c) {
        c.drawRect(0, 0, getWidth(), getHeight(), bgPaint);
        boolean shiftOn = state.isModActive(HidKeyboard.MOD_LSHIFT)
                || state.isModActive(HidKeyboard.MOD_RSHIFT);

        labelPaint.setTextSize(labelSize);
        subPaint.setTextSize(labelSize * 0.5f);
        Paint.FontMetrics fm = labelPaint.getFontMetrics();

        for (int i = 0; i < keys.size(); i++) {
            KeyboardLayout.Key k = keys.get(i);
            RectF r = rects.get(i);
            boolean pressed = pointerKeys.indexOfValue(k) >= 0;
            boolean modOn = k.isModifier() && state.isModActive(k.mod);

            int color;
            if (pressed || modOn) {
                color = COLOR_KEY_DOWN;
            } else if (k.style == KeyboardLayout.STYLE_ACCENT) {
                color = COLOR_KEY_MOD;
            } else if (k.style == KeyboardLayout.STYLE_SPECIAL) {
                color = COLOR_KEY_SPECIAL;
            } else {
                color = COLOR_KEY;
            }
            keyPaint.setColor(color);
            float radius = Math.min(r.width(), r.height()) * 0.16f;
            c.drawRoundRect(r, radius, radius, keyPaint);

            String label = k.label;
            if (label != null && label.length() == 1 && Character.isLetter(label.charAt(0))) {
                label = label.toUpperCase();
            }
            if (label != null && label.length() > 0) {
                float baseline = r.centerY() - (fm.ascent + fm.descent) / 2f;
                c.drawText(label, r.centerX(), baseline, labelPaint);
            }

            if (k.sub != null && shiftOn) {
                c.drawText(k.sub, r.right - pad * 2f, r.top + subPaint.getTextSize() * 1.35f, subPaint);
            }
        }
    }
}
