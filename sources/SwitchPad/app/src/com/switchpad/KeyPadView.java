package com.switchpad;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.AttributeSet;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;

/**
 * 屏幕键盘：自绘 QWERTY，直接产出 HID 键盘报告。
 *
 * 设计取舍：
 * <ul>
 *   <li>不用系统 IME —— 那样只能往本机输入框打字，没法把按键送往电脑；</li>
 *   <li>修饰键是「锁存」的：点一下就保持按下，再点取消；Shift 是一次性修饰，
 *       点两次则锁定，符合软键盘习惯；</li>
 *   <li>支持多点：一根手指按住 Shift 的同时另一根手指敲字母也没问题。</li>
 * </ul>
 */
public final class KeyPadView extends View {

    public interface HidBridge {
        void pressKey(int usage);

        void releaseKey(int usage);

        void setModifier(int modIndex, boolean down);

        void releaseAllModifiers();

        void sendConsumer(int usage);

        void sendChord(int modifierBits, int[] usages);
    }

    private static final int T_CHAR = 0;
    private static final int T_MOD = 1;
    private static final int T_CONSUMER = 2;
    private static final int T_CHORD = 3;
    private static final int T_FN = 4;

    private static final long SHIFT_LOCK_WINDOW_MS = 320;

    private static final class Key {
        final String label;
        final int type;
        final int usage;
        final int modIndex;
        final int consumer;
        final int chordBits;
        final int[] chordKeys;
        final float weight;

        Key(String label, int type, int usage, int modIndex, int consumer,
            int chordBits, int[] chordKeys, float weight) {
            this.label = label;
            this.type = type;
            this.usage = usage;
            this.modIndex = modIndex;
            this.consumer = consumer;
            this.chordBits = chordBits;
            this.chordKeys = chordKeys;
            this.weight = weight;
        }
    }

    private static final class Press {
        final int row;
        final int col;
        final Key key;

        Press(int row, int col, Key key) {
            this.row = row;
            this.col = col;
            this.key = key;
        }
    }

    /* ------------------------- 键位表 ------------------------- */

    private static Key ch(String label, int usage) {
        return new Key(label, T_CHAR, usage, -1, 0, 0, null, 1f);
    }

    private static Key ch(String label, int usage, float weight) {
        return new Key(label, T_CHAR, usage, -1, 0, 0, null, weight);
    }

    private static Key mod(String label, int modIndex) {
        return new Key(label, T_MOD, 0, modIndex, 0, 0, null, 1.25f);
    }

    private static Key mod(String label, int modIndex, float weight) {
        return new Key(label, T_MOD, 0, modIndex, 0, 0, null, weight);
    }

    private static Key med(String label, int consumer) {
        return new Key(label, T_CONSUMER, 0, -1, consumer, 0, null, 1.2f);
    }

    private final Paint keyPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint edgePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private final SparseArray<Press> presses = new SparseArray<Press>();
    private final boolean[] latched = new boolean[8];
    private final boolean[] oneShotLocked = new boolean[8];
    private final long[] lastTapTime = new long[8];

    private Key[][] rows;
    private RectF[][] rects;
    private boolean fnActive;

    private HidBridge bridge;
    private boolean haptics;

    public KeyPadView(Context context) {
        this(context, null);
    }

    public KeyPadView(Context context, AttributeSet attrs) {
        super(context, attrs);
        fnActive = Prefs.fnRow(context);
        haptics = Prefs.haptics(context);
        rows = buildRows();

        keyPaint.setStyle(Paint.Style.FILL);
        edgePaint.setStyle(Paint.Style.STROKE);
        edgePaint.setStrokeWidth(Ui.dp(context, 1));
        textPaint.setColor(Ui.TEXT);
        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setFakeBoldText(false);
        setFocusable(false);
    }

    public void setBridge(HidBridge bridge) {
        this.bridge = bridge;
    }

    public void setHaptics(boolean on) {
        haptics = on;
    }

    /** 连接断开或切换主机时，把锁存的修饰键全部放开，避免电脑侧按键卡住。 */
    public void resetLatches() {
        for (int i = 0; i < latched.length; i++) {
            latched[i] = false;
            oneShotLocked[i] = false;
        }
        presses.clear();
        if (bridge != null) bridge.releaseAllModifiers();
        invalidate();
    }

    /* ------------------------- 行定义 ------------------------- */

    private Key[][] buildRows() {
        Key[][] r = new Key[5][];

        if (fnActive) {
            r[0] = new Key[]{
                    ch("Esc", HidUsages.K_ESC),
                    ch("F1", HidUsages.K_F1), ch("F2", HidUsages.K_F1 + 1),
                    ch("F3", HidUsages.K_F1 + 2), ch("F4", HidUsages.K_F1 + 3),
                    ch("F5", HidUsages.K_F1 + 4), ch("F6", HidUsages.K_F1 + 5),
                    ch("F7", HidUsages.K_F1 + 6), ch("F8", HidUsages.K_F1 + 7),
                    ch("F9", HidUsages.K_F1 + 8), ch("F10", HidUsages.K_F1 + 9),
                    ch("F11", HidUsages.K_F1 + 10), ch("F12", HidUsages.K_F1 + 11),
                    ch("Del", HidUsages.K_DELETE, 1.4f)
            };
        } else {
            r[0] = new Key[]{
                    ch("`", HidUsages.K_GRAVE),
                    ch("1", HidUsages.digit('1')), ch("2", HidUsages.digit('2')),
                    ch("3", HidUsages.digit('3')), ch("4", HidUsages.digit('4')),
                    ch("5", HidUsages.digit('5')), ch("6", HidUsages.digit('6')),
                    ch("7", HidUsages.digit('7')), ch("8", HidUsages.digit('8')),
                    ch("9", HidUsages.digit('9')), ch("0", HidUsages.digit('0')),
                    ch("-", HidUsages.K_MINUS), ch("=", HidUsages.K_EQUAL),
                    ch("⌫", HidUsages.K_BACKSPACE, 1.5f)
            };
        }

        r[1] = new Key[]{
                ch("⇥", HidUsages.K_TAB, 1.35f),
                ch("q", HidUsages.letter('q')), ch("w", HidUsages.letter('w')),
                ch("e", HidUsages.letter('e')), ch("r", HidUsages.letter('r')),
                ch("t", HidUsages.letter('t')), ch("y", HidUsages.letter('y')),
                ch("u", HidUsages.letter('u')), ch("i", HidUsages.letter('i')),
                ch("o", HidUsages.letter('o')), ch("p", HidUsages.letter('p')),
                ch("[", HidUsages.K_LBRACKET), ch("]", HidUsages.K_RBRACKET),
                ch("\\", HidUsages.K_BACKSLASH, 1.35f)
        };

        r[2] = new Key[]{
                ch("⇪", HidUsages.K_CAPS_LOCK, 1.5f),
                ch("a", HidUsages.letter('a')), ch("s", HidUsages.letter('s')),
                ch("d", HidUsages.letter('d')), ch("f", HidUsages.letter('f')),
                ch("g", HidUsages.letter('g')), ch("h", HidUsages.letter('h')),
                ch("j", HidUsages.letter('j')), ch("k", HidUsages.letter('k')),
                ch("l", HidUsages.letter('l')),
                ch(";", HidUsages.K_SEMICOLON), ch("'", HidUsages.K_APOSTROPHE),
                ch("⏎", HidUsages.K_ENTER, 1.75f)
        };

        r[3] = new Key[]{
                mod("⇧", HidUsages.MOD_LSHIFT, 1.7f),
                ch("z", HidUsages.letter('z')), ch("x", HidUsages.letter('x')),
                ch("c", HidUsages.letter('c')), ch("v", HidUsages.letter('v')),
                ch("b", HidUsages.letter('b')), ch("n", HidUsages.letter('n')),
                ch("m", HidUsages.letter('m')),
                ch(",", HidUsages.K_COMMA), ch(".", HidUsages.K_PERIOD),
                ch("/", HidUsages.K_SLASH),
                mod("⇧", HidUsages.MOD_RSHIFT, 1.7f)
        };

        r[4] = new Key[]{
                mod("Ctrl", HidUsages.MOD_LCTRL, 1.3f),
                mod("⊞", HidUsages.MOD_LGUI, 1.1f),
                mod("Alt", HidUsages.MOD_LALT, 1.3f),
                ch("空格", HidUsages.K_SPACE, 3.2f),
                mod("Alt", HidUsages.MOD_RALT, 1.3f),
                new Key("Fn", T_FN, 0, -1, 0, 0, null, 1.0f),
                ch("◀", HidUsages.K_LEFT), ch("▲", HidUsages.K_UP),
                ch("▼", HidUsages.K_DOWN), ch("▶", HidUsages.K_RIGHT)
        };

        return r;
    }

    /* ------------------------- 布局 ------------------------- */

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        layoutKeys(w, h);
    }

    private void layoutKeys(int w, int h) {
        int n = rows.length;
        if (w <= 0 || h <= 0 || n == 0) return;
        float gap = Ui.dp(getContext(), 2.5f);
        float rowH = (float) h / n;
        rects = new RectF[n][];
        for (int i = 0; i < n; i++) {
            Key[] row = rows[i];
            float total = 0f;
            for (Key k : row) total += k.weight;
            rects[i] = new RectF[row.length];
            float x = 0f;
            for (int j = 0; j < row.length; j++) {
                float kw = w * row[j].weight / total;
                rects[i][j] = new RectF(
                        x + gap, i * rowH + gap, x + kw - gap, (i + 1) * rowH - gap);
                x += kw;
            }
        }
    }

    /* ------------------------- 绘制 ------------------------- */

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (rects == null) {
            layoutKeys(getWidth(), getHeight());
            if (rects == null) return;
        }
        float radius = Ui.dp(getContext(), 7);
        for (int i = 0; i < rows.length; i++) {
            for (int j = 0; j < rows[i].length; j++) {
                Key k = rows[i][j];
                RectF r = rects[i][j];
                boolean pressed = isPressed(i, j);
                boolean lit = isLatched(k) || (k.type == T_FN && fnActive);

                int bg;
                int fg = Ui.TEXT;
                if (pressed) {
                    bg = Ui.ACCENT;
                    fg = 0xFF06121E;
                } else if (lit) {
                    bg = Ui.alpha(Ui.ACCENT, 0.28f);
                    fg = Ui.ACCENT;
                } else if (k.type == T_MOD) {
                    bg = 0xFF313E52;
                } else if (k.type == T_CHORD) {
                    bg = 0xFF2A3345;
                } else {
                    bg = Ui.PANEL_2;
                }

                keyPaint.setColor(bg);
                canvas.drawRoundRect(r, radius, radius, keyPaint);
                edgePaint.setColor(Ui.alpha(Ui.STROKE, 0.9f));
                canvas.drawRoundRect(r, radius, radius, edgePaint);

                float base = Math.min(r.height() * 0.46f, r.width() * 0.55f);
                if (base < Ui.dp(getContext(), 8)) base = Ui.dp(getContext(), 8);
                textPaint.setColor(fg);
                textPaint.setTextSize(base);
                float tw = textPaint.measureText(k.label);
                float maxW = r.width() - Ui.dp(getContext(), 4);
                if (tw > maxW && tw > 0f) {
                    textPaint.setTextSize(Math.max(Ui.dp(getContext(), 8), base * maxW / tw));
                }
                float ty = r.centerY() - (textPaint.ascent() + textPaint.descent()) / 2f;
                canvas.drawText(k.label, r.centerX(), ty, textPaint);
            }
        }
    }

    private boolean isPressed(int row, int col) {
        for (int i = 0; i < presses.size(); i++) {
            Press p = presses.valueAt(i);
            if (p.row == row && p.col == col) return true;
        }
        return false;
    }

    private boolean isLatched(Key k) {
        return k.type == T_MOD && latched[k.modIndex];
    }

    /* ------------------------- 触摸 ------------------------- */

    @Override
    public boolean onTouchEvent(MotionEvent ev) {
        if (bridge == null) return false;
        switch (ev.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                pressAt(ev.getPointerId(0), ev.getX(0), ev.getY(0));
                return true;
            case MotionEvent.ACTION_POINTER_DOWN: {
                int idx = ev.getActionIndex();
                pressAt(ev.getPointerId(idx), ev.getX(idx), ev.getY(idx));
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                // 滑指换键：手指滑到另一个键上时，松开旧键、按下新键
                for (int i = 0; i < ev.getPointerCount(); i++) {
                    int id = ev.getPointerId(i);
                    Press p = presses.get(id);
                    if (p == null) continue;
                    int[] hit = hitTest(ev.getX(i), ev.getY(i));
                    if (hit == null) continue;
                    if (hit[0] != p.row || hit[1] != p.col) {
                        releasePress(p);
                        presses.remove(id);
                        pressAt(id, ev.getX(i), ev.getY(i));
                    }
                }
                return true;
            }
            case MotionEvent.ACTION_POINTER_UP: {
                int idx = ev.getActionIndex();
                int id = ev.getPointerId(idx);
                Press p = presses.get(id);
                if (p != null) {
                    releasePress(p);
                    presses.remove(id);
                }
                return true;
            }
            case MotionEvent.ACTION_UP: {
                for (int i = 0; i < presses.size(); i++) {
                    releasePress(presses.valueAt(i));
                }
                presses.clear();
                return true;
            }
            case MotionEvent.ACTION_CANCEL:
                presses.clear();
                invalidate();
                return true;
            default:
                return true;
        }
    }

    private int[] hitTest(float x, float y) {
        if (rects == null) return null;
        for (int i = 0; i < rects.length; i++) {
            for (int j = 0; j < rects[i].length; j++) {
                if (rects[i][j].contains(x, y)) return new int[]{i, j};
            }
        }
        return null;
    }

    private void pressAt(int pointerId, float x, float y) {
        if (bridge == null) return;
        int[] hit = hitTest(x, y);
        if (hit == null) return;
        Key k = rows[hit[0]][hit[1]];
        presses.put(pointerId, new Press(hit[0], hit[1], k));
        vibrate();

        switch (k.type) {
            case T_FN: {
                fnActive = !fnActive;
                Prefs.setFnRow(getContext(), fnActive);
                rows = buildRows();
                layoutKeys(getWidth(), getHeight());
                break;
            }
            case T_MOD:
                toggleModifier(k.modIndex);
                break;
            case T_CHAR:
                applyLatchedModifiers();
                bridge.pressKey(k.usage);
                break;
            case T_CONSUMER:
                bridge.sendConsumer(k.consumer);
                break;
            case T_CHORD:
                bridge.sendChord(k.chordBits, k.chordKeys);
                break;
            default:
                break;
        }
        invalidate();
    }

    private void releasePress(Press p) {
        if (bridge == null || p == null) return;
        if (p.key.type == T_CHAR) {
            bridge.releaseKey(p.key.usage);
            clearOneShotModifiers();
        }
        invalidate();
    }

    private void toggleModifier(int idx) {
        long now = android.os.SystemClock.uptimeMillis();
        boolean isShift = idx == HidUsages.MOD_LSHIFT || idx == HidUsages.MOD_RSHIFT;

        if (isShift && latched[idx] && now - lastTapTime[idx] < SHIFT_LOCK_WINDOW_MS) {
            // 双击 Shift 锁定
            oneShotLocked[idx] = true;
        } else if (isShift && latched[idx] && oneShotLocked[idx]) {
            oneShotLocked[idx] = false;
        }
        lastTapTime[idx] = now;

        latched[idx] = !latched[idx];
        if (!latched[idx]) oneShotLocked[idx] = false;
        bridge.setModifier(idx, latched[idx]);
    }

    private void applyLatchedModifiers() {
        for (int i = 0; i < latched.length; i++) {
            if (latched[i]) bridge.setModifier(i, true);
        }
    }

    private void clearOneShotModifiers() {
        for (int i = 0; i < latched.length; i++) {
            if (!latched[i]) continue;
            boolean isShift = i == HidUsages.MOD_LSHIFT || i == HidUsages.MOD_RSHIFT;
            if (!isShift) continue;
            if (oneShotLocked[i]) continue;
            latched[i] = false;
            bridge.setModifier(i, false);
        }
    }

    private void vibrate() {
        if (!haptics) return;
        try {
            performHapticFeedback(android.view.HapticFeedbackConstants.VIRTUAL_KEY);
        } catch (Exception ignored) {
        }
    }
}
