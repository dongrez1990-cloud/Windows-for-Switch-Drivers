package com.paopao.switchkey;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.SystemClock;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

/**
 * 触控板：一根手指拖动移动指针，轻点左键，两指轻点右键，两指拖动滚轮，
 * 底部三个实体感按键对应左/中/右键。
 */
public class MousePadView extends View {

    public interface MouseSink {
        void onMouse(int buttons, int dx, int dy, int wheel);
    }

    public static final int BTN_LEFT = 0x01;
    public static final int BTN_RIGHT = 0x02;
    public static final int BTN_MIDDLE = 0x04;

    private static final int COLOR_BG = 0xFF14161A;
    private static final int COLOR_PAD = 0xFF1E232B;
    private static final int COLOR_PAD_EDGE = 0xFF2B323C;
    private static final int COLOR_KEY = 0xFF2B2F37;
    private static final int COLOR_KEY_DOWN = 0xFF4C9AFF;
    private static final int COLOR_TEXT = 0xFFE9ECF1;
    private static final int COLOR_DIM = 0xFF7A828D;

    private static final float SPEED = 1.25f;
    private static final float SCROLL_STEP_PX = 22f;
    private static final int MAX_DELTA = 127;

    private MouseSink sink;

    private final Paint bgPaint = new Paint();
    private final Paint padPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint edgePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint keyPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint hintPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private final RectF padRect = new RectF();
    private final RectF[] btnRects = new RectF[] { new RectF(), new RectF(), new RectF() };
    private final String[] btnLabels = new String[] { "左键", "中键", "右键" };
    private final int[] btnBits = new int[] { BTN_LEFT, BTN_MIDDLE, BTN_RIGHT };

    private int heldButtons;
    private int activeButtonIndex = -1;

    private float accX;
    private float accY;
    private float accWheel;
    private float lastX;
    private float lastY;
    private long downTime;
    private boolean moved;
    private boolean twoFinger;

    private float pad;
    private float labelSize;

    public MousePadView(Context ctx) {
        super(ctx);
        setClickable(true);
        setFocusable(true);
        bgPaint.setColor(COLOR_BG);
        padPaint.setColor(COLOR_PAD);
        edgePaint.setColor(COLOR_PAD_EDGE);
        edgePaint.setStyle(Paint.Style.STROKE);
        textPaint.setColor(COLOR_TEXT);
        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setTypeface(Typeface.DEFAULT_BOLD);
        hintPaint.setColor(COLOR_DIM);
        hintPaint.setTextAlign(Paint.Align.CENTER);
        hintPaint.setTypeface(Typeface.DEFAULT);
    }

    public void setSink(MouseSink s) {
        this.sink = s;
    }

    public void releaseAll() {
        activeButtonIndex = -1;
        heldButtons = 0;
        if (sink != null) {
            sink.onMouse(0, 0, 0, 0);
        }
        invalidate();
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        if (w <= 0 || h <= 0) {
            return;
        }
        pad = Math.max(6f, w * 0.02f);
        labelSize = Math.min(w * 0.045f, h * 0.09f);
        edgePaint.setStrokeWidth(Math.max(2f, pad * 0.4f));

        float padBottom = h * 0.70f;
        padRect.set(pad, pad, w - pad, padBottom);

        float btnTop = padBottom + pad;
        float btnBottom = h - pad;
        float btnW = (w - pad * 4f) / 3f;
        for (int i = 0; i < 3; i++) {
            float left = pad + i * (btnW + pad);
            btnRects[i].set(left, btnTop, left + btnW, btnBottom);
        }
    }

    // ---------------- 触摸 ----------------

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: {
                lastX = e.getX();
                lastY = e.getY();
                downTime = SystemClock.uptimeMillis();
                moved = false;
                twoFinger = false;
                int idx = buttonIndexAt(lastX, lastY);
                if (idx >= 0) {
                    activeButtonIndex = idx;
                    performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
                    heldButtons |= btnBits[idx];
                    send(heldButtons, 0, 0, 0);
                    invalidate();
                }
                return true;
            }
            case MotionEvent.ACTION_POINTER_DOWN: {
                // 两指按下：进入滚动手势，不再判定为点击
                twoFinger = true;
                moved = true;
                lastX = centroid(e, true);
                lastY = centroid(e, false);
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                if (activeButtonIndex >= 0) {
                    return true;
                }
                if (e.getPointerCount() >= 2) {
                    float cy = centroid(e, false);
                    float dy = cy - lastY;
                    lastX = centroid(e, true);
                    lastY = cy;
                    scroll(dy);
                } else {
                    float dx = e.getX() - lastX;
                    float dy = e.getY() - lastY;
                    lastX = e.getX();
                    lastY = e.getY();
                    if (Math.abs(dx) + Math.abs(dy) > 3f) {
                        moved = true;
                    }
                    move(dx, dy);
                }
                return true;
            }
            case MotionEvent.ACTION_POINTER_UP: {
                // 抬起一指后剩下的手指不要误触发点击
                moved = true;
                return true;
            }
            case MotionEvent.ACTION_UP: {
                if (activeButtonIndex >= 0) {
                    heldButtons &= ~btnBits[activeButtonIndex];
                    send(heldButtons, 0, 0, 0);
                    activeButtonIndex = -1;
                    invalidate();
                    return true;
                }
                long dt = SystemClock.uptimeMillis() - downTime;
                if (!moved && dt < 260) {
                    performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
                    click(twoFinger ? BTN_RIGHT : BTN_LEFT);
                }
                moved = false;
                twoFinger = false;
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

    private static float centroid(MotionEvent e, boolean xAxis) {
        int n = e.getPointerCount();
        if (n <= 0) {
            return 0f;
        }
        float sum = 0f;
        for (int i = 0; i < n; i++) {
            sum += xAxis ? e.getX(i) : e.getY(i);
        }
        return sum / n;
    }

    private int buttonIndexAt(float x, float y) {
        for (int i = 0; i < btnRects.length; i++) {
            if (btnRects[i].contains(x, y)) {
                return i;
            }
        }
        return -1;
    }

    // ---------------- 输入 ----------------

    private void move(float dx, float dy) {
        accX += dx * SPEED;
        accY += dy * SPEED;
        int ix = (int) accX;
        int iy = (int) accY;
        if (ix == 0 && iy == 0) {
            return;
        }
        accX -= ix;
        accY -= iy;
        if (ix > MAX_DELTA) {
            ix = MAX_DELTA;
        } else if (ix < -MAX_DELTA) {
            ix = -MAX_DELTA;
        }
        if (iy > MAX_DELTA) {
            iy = MAX_DELTA;
        } else if (iy < -MAX_DELTA) {
            iy = -MAX_DELTA;
        }
        send(heldButtons, ix, iy, 0);
    }

    private void scroll(float dy) {
        accWheel += dy;
        int notches = (int) (accWheel / SCROLL_STEP_PX);
        if (notches == 0) {
            return;
        }
        accWheel -= notches * SCROLL_STEP_PX;
        // 手指向下拖 = 内容向下滚 = 滚轮负值
        send(heldButtons, 0, 0, -notches);
    }

    private void click(final int bit) {
        send(bit, 0, 0, 0);
        // 稍等一下再抬起，避免同帧的 down/up 被主机合并掉
        postDelayed(new Runnable() {
            @Override
            public void run() {
                send(0, 0, 0, 0);
            }
        }, 15L);
    }

    private void send(int buttons, int dx, int dy, int wheel) {
        if (sink != null) {
            sink.onMouse(buttons, dx, dy, wheel);
        }
    }

    // ---------------- 绘制 ----------------

    @Override
    protected void onDraw(Canvas c) {
        c.drawRect(0, 0, getWidth(), getHeight(), bgPaint);

        float radius = Math.min(getWidth(), getHeight()) * 0.03f;
        c.drawRoundRect(padRect, radius, radius, padPaint);
        c.drawRoundRect(padRect, radius, radius, edgePaint);

        hintPaint.setTextSize(Math.min(getWidth() * 0.036f, getHeight() * 0.06f));
        float cx = padRect.centerX();
        float cy = padRect.centerY();
        c.drawText("拖动 = 移动指针", cx, cy - hintPaint.getTextSize() * 1.6f, hintPaint);
        c.drawText("轻点 = 左键　两指轻点 = 右键", cx, cy, hintPaint);
        c.drawText("两指拖动 = 滚轮", cx, cy + hintPaint.getTextSize() * 1.6f, hintPaint);

        textPaint.setTextSize(labelSize);
        Paint.FontMetrics fm = textPaint.getFontMetrics();
        for (int i = 0; i < btnRects.length; i++) {
            boolean down = (heldButtons & btnBits[i]) != 0;
            keyPaint.setColor(down ? COLOR_KEY_DOWN : COLOR_KEY);
            RectF r = btnRects[i];
            c.drawRoundRect(r, radius, radius, keyPaint);
            float baseline = r.centerY() - (fm.ascent + fm.descent) / 2f;
            c.drawText(btnLabels[i], r.centerX(), baseline, textPaint);
        }
    }
}
