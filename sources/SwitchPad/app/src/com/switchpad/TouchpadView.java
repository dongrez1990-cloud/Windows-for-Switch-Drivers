package com.switchpad;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.AttributeSet;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;

/**
 * 触控板：把手机屏幕当作笔记本的触摸板。
 *
 * 手势约定（与 Windows 触控板习惯一致）：
 * <ul>
 *   <li>单指拖动 — 移动指针（带亚像素累加 + 速度加速）</li>
 *   <li>单指轻点 — 左键单击</li>
 *   <li>双指轻点 — 右键单击</li>
 *   <li>三指轻点 — 中键单击</li>
 *   <li>双指上下滑 — 滚轮</li>
 *   <li>单指长按后拖动 — 拖拽（按住左键）</li>
 * </ul>
 */
public final class TouchpadView extends View {

    public interface PadBridge {
        void moveBy(float dx, float dy, float dtMs);

        void wheelBy(float dyPx);

        void setMouseButtons(int mask);

        void tapMouseButton(int mask);
    }

    private static final int TAP_TIMEOUT_MS = 220;
    private static final int LONG_PRESS_MS = 340;

    private final Paint bgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint dotPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ringPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint hintPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF bgRect = new RectF();

    private PadBridge bridge;
    private float slop;

    private int activePointerId = -1;
    private float lastX;
    private float lastY;
    private long lastTime;
    private float downX;
    private float downY;
    private long downTime;

    private float pendX;
    private float pendY;
    private boolean moved;
    private boolean scrolling;
    private float centX;
    private float centY;
    private boolean multiMoved;
    private int maxPointers;
    private boolean dragActive;

    private float touchX;
    private float touchY;
    private boolean touching;
    private float ringPhase; // 0..1 点击涟漪

    private boolean haptics;

    private final Runnable longPressDrag = new Runnable() {
        @Override
        public void run() {
            if (bridge == null || touching == false) return;
            if (maxPointers == 1 && !moved && !scrolling && !dragActive) {
                dragActive = true;
                bridge.setMouseButtons(HidUsages.BTN_LEFT);
                vibrate(HapticFeedbackConstants.LONG_PRESS);
                invalidate();
            }
        }
    };

    public TouchpadView(Context context) {
        this(context, null);
    }

    public TouchpadView(Context context, AttributeSet attrs) {
        super(context, attrs);
        slop = Math.max(6f, ViewConfiguration.get(context).getScaledTouchSlop() * 0.7f);
        haptics = Prefs.haptics(context);

        bgPaint.setColor(Ui.PANEL);
        bgPaint.setStyle(Paint.Style.FILL);
        dotPaint.setColor(Ui.alpha(Ui.ACCENT, 0.55f));
        dotPaint.setStyle(Paint.Style.FILL);
        ringPaint.setColor(Ui.alpha(Ui.ACCENT, 0.9f));
        ringPaint.setStyle(Paint.Style.STROKE);
        ringPaint.setStrokeWidth(Ui.dp(context, 1.5f));
        hintPaint.setColor(Ui.alpha(Ui.TEXT_DIM, 0.75f));
        hintPaint.setTextSize(Ui.dp(context, 11));
        hintPaint.setTextAlign(Paint.Align.CENTER);
    }

    public void setBridge(PadBridge bridge) {
        this.bridge = bridge;
    }

    public void setHaptics(boolean on) {
        haptics = on;
    }

    public void setSlopScale(float factor) {
        slop = Math.max(6f, ViewConfiguration.get(getContext()).getScaledTouchSlop() * factor);
    }

    /* ------------------------------ 触摸 ------------------------------ */

    @Override
    public boolean onTouchEvent(MotionEvent ev) {
        if (bridge == null) return false;
        switch (ev.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                onDown(ev);
                return true;
            case MotionEvent.ACTION_POINTER_DOWN:
                onPointerDown(ev);
                return true;
            case MotionEvent.ACTION_MOVE:
                onMove(ev);
                return true;
            case MotionEvent.ACTION_POINTER_UP:
                onPointerUp(ev);
                return true;
            case MotionEvent.ACTION_UP:
                onUp(ev);
                return true;
            case MotionEvent.ACTION_CANCEL:
                onCancel();
                return true;
            default:
                return true;
        }
    }

    private void onDown(MotionEvent ev) {
        removeCallbacks(longPressDrag);
        activePointerId = ev.getPointerId(0);
        downX = lastX = touchX = ev.getX(0);
        downY = lastY = touchY = ev.getY(0);
        downTime = SystemClockNow();
        lastTime = downTime;
        pendX = 0f;
        pendY = 0f;
        moved = false;
        scrolling = false;
        multiMoved = false;
        dragActive = false;
        maxPointers = 1;
        touching = true;
        ringPhase = 0f;
        if (Prefs.longPressDrag(getContext())) {
            postDelayed(longPressDrag, LONG_PRESS_MS);
        }
        invalidate();
    }

    private void onPointerDown(MotionEvent ev) {
        if (bridge == null) return;
        removeCallbacks(longPressDrag);
        maxPointers = Math.max(maxPointers, ev.getPointerCount());
        if (dragActive) {
            dragActive = false;
            bridge.setMouseButtons(0);
        }
        if (ev.getPointerCount() >= 2) {
            scrolling = true;
            centX = centroidX(ev);
            centY = centroidY(ev);
        }
        invalidate();
    }

    private void onMove(MotionEvent ev) {
        if (bridge == null) return;
        if (ev.getPointerCount() >= 2) {
            float cx = centroidX(ev);
            float cy = centroidY(ev);
            float dx = cx - centX;
            float dy = cy - centY;
            if (!multiMoved && (Math.abs(dx) > slop || Math.abs(dy) > slop)) {
                multiMoved = true;
            }
            if (dy != 0f) bridge.wheelBy(dy);
            centX = cx;
            centY = cy;
            touchX = cx;
            touchY = cy;
            invalidate();
            return;
        }

        int idx = ev.findPointerIndex(activePointerId);
        if (idx < 0) idx = 0;
        float x = ev.getX(idx);
        float y = ev.getY(idx);
        touchX = x;
        touchY = y;

        float dx = x - lastX;
        float dy = y - lastY;
        lastX = x;
        lastY = y;

        long now = ev.getEventTime();
        float dt = now - lastTime;
        if (dt < 1f) dt = 1f;
        lastTime = now;

        if (!moved) {
            pendX += dx;
            pendY += dy;
            if ((float) Math.hypot(pendX, pendY) > slop) {
                moved = true;
                removeCallbacks(longPressDrag);
                // 越过阈值后一次性把攒下的位移发出去，避免"黏滞后突然一跳"
                bridge.moveBy(pendX, pendY, Math.max(dt, 8f));
                pendX = 0f;
                pendY = 0f;
            }
            invalidate();
            return;
        }

        bridge.moveBy(dx, dy, dt);
        invalidate();
    }

    private void onPointerUp(MotionEvent ev) {
        if (bridge == null) return;
        int remaining = ev.getPointerCount() - 1;
        if (remaining <= 0) return;

        if (dragActive) {
            dragActive = false;
            bridge.setMouseButtons(0);
        }

        if (remaining >= 2) {
            // 三指以上抬走一根：重算质心，避免滚轮因为质心跳动而突然窜一下
            centX = centroidXExcluding(ev, ev.getActionIndex());
            centY = centroidYExcluding(ev, ev.getActionIndex());
            return;
        }

        // 只剩一根手指：结束滚动手势，且此后不再判为轻点
        scrolling = false;
        multiMoved = true;
        int upIdx = ev.getActionIndex();
        int keep = upIdx == 0 ? 1 : 0;
        activePointerId = ev.getPointerId(keep);
        lastX = ev.getX(keep);
        lastY = ev.getY(keep);
        lastTime = ev.getEventTime();
    }

    private void onUp(MotionEvent ev) {
        removeCallbacks(longPressDrag);
        if (bridge == null) {
            touching = false;
            activePointerId = -1;
            maxPointers = 0;
            moved = false;
            scrolling = false;
            invalidate();
            return;
        }
        touching = false;
        long duration = SystemClockNow() - downTime;

        if (dragActive) {
            dragActive = false;
            bridge.setMouseButtons(0);
            vibrate(HapticFeedbackConstants.VIRTUAL_KEY);
            invalidate();
            return;
        }

        boolean quick = duration < TAP_TIMEOUT_MS;
        if (maxPointers >= 2) {
            if (!multiMoved && quick) {
                bridge.tapMouseButton(maxPointers >= 3 ? HidUsages.BTN_MIDDLE : HidUsages.BTN_RIGHT);
                vibrate(HapticFeedbackConstants.VIRTUAL_KEY);
                ringPhase = 1f;
            }
        } else if (!moved && quick && Prefs.tapToClick(getContext())) {
            bridge.tapMouseButton(HidUsages.BTN_LEFT);
            vibrate(HapticFeedbackConstants.VIRTUAL_KEY);
            ringPhase = 1f;
        }

        activePointerId = -1;
        maxPointers = 0;
        moved = false;
        scrolling = false;
        invalidate();
    }

    private void onCancel() {
        removeCallbacks(longPressDrag);
        if (dragActive) {
            dragActive = false;
            bridge.setMouseButtons(0);
        }
        touching = false;
        activePointerId = -1;
        maxPointers = 0;
        moved = false;
        scrolling = false;
        invalidate();
    }

    private static long SystemClockNow() {
        return android.os.SystemClock.uptimeMillis();
    }

    private static float centroidX(MotionEvent ev) {
        float s = 0f;
        int n = ev.getPointerCount();
        for (int i = 0; i < n; i++) s += ev.getX(i);
        return s / n;
    }

    private static float centroidY(MotionEvent ev) {
        float s = 0f;
        int n = ev.getPointerCount();
        for (int i = 0; i < n; i++) s += ev.getY(i);
        return s / n;
    }

    private static float centroidXExcluding(MotionEvent ev, int skip) {
        float s = 0f;
        int n = 0;
        for (int i = 0; i < ev.getPointerCount(); i++) {
            if (i == skip) continue;
            s += ev.getX(i);
            n++;
        }
        return n == 0 ? 0f : s / n;
    }

    private static float centroidYExcluding(MotionEvent ev, int skip) {
        float s = 0f;
        int n = 0;
        for (int i = 0; i < ev.getPointerCount(); i++) {
            if (i == skip) continue;
            s += ev.getY(i);
            n++;
        }
        return n == 0 ? 0f : s / n;
    }

    private void vibrate(int kind) {
        if (!haptics) return;
        try {
            performHapticFeedback(kind);
        } catch (Exception ignored) {
        }
    }

    /* ------------------------------ 绘制 ------------------------------ */

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        int w = getWidth();
        int h = getHeight();
        int pad = Ui.dp(getContext(), 8);
        bgRect.set(pad, pad, w - pad, h - pad);
        canvas.drawRoundRect(bgRect, Ui.dp(getContext(), 16), Ui.dp(getContext(), 16), bgPaint);

        if (!touching && ringPhase <= 0f) {
            canvas.drawText("触控板 · 单指移动/轻点 · 双指滚动/右键 · 长按拖动 · 速度在 ⚙",
                    w / 2f, h / 2f, hintPaint);
            return;
        }

        float r = Ui.dp(getContext(), dragActive ? 26 : 18);
        dotPaint.setAlpha(dragActive ? 200 : 130);
        canvas.drawCircle(touchX, touchY, r, dotPaint);

        if (dragActive) {
            ringPaint.setAlpha(230);
            canvas.drawCircle(touchX, touchY, Ui.dp(getContext(), 34), ringPaint);
        }

        if (ringPhase > 0f) {
            float rr = Ui.dp(getContext(), 18) + (1f - ringPhase) * Ui.dp(getContext(), 40);
            ringPaint.setAlpha((int) (200 * ringPhase));
            canvas.drawCircle(touchX, touchY, rr, ringPaint);
            ringPhase -= 0.09f;
            if (ringPhase < 0f) ringPhase = 0f;
            if (ringPhase > 0f) postInvalidateDelayed(16);
        }
    }
}
