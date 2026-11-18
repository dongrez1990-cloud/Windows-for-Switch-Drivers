package com.paopao.switchkey;

/**
 * 指针报告合并器（纯 Java，不依赖 Android，可上 JVM 单测）。
 *
 * <p>为什么需要：Switch 里的 BCM4356A3 蓝牙挂在 UART-D 上，出厂 ROM 固件只有
 * 115200 bps（要打 Broadcom 的 hcd 补丁才能上 3 Mbaud，见 switchbt.c）。手指在触控板上
 * 滑动时系统每秒能吐 100~200 个 MotionEvent，若每个都发一帧鼠标报告，
 * 每条 ACL 报文都要过一遍 115200 的串口，链路会排队、指针发飘甚至丢包。
 *
 * <p>所以这里把固定时间窗内的位移累加成一帧；但「按键变化」和「滚轮」永远立刻发，
 * 点击手感不能等。
 */
public final class PointerPump {

    public interface Sink {
        void onMouse(int buttons, int dx, int dy, int wheel);
    }

    /** 时间与调度抽象，便于 JVM 单测注入假时钟 */
    public interface Clock {
        long now();

        void postDelayed(Runnable r, long delayMs);

        void cancel(Runnable r);
    }

    /** 60Hz：默认，视觉跟手 */
    public static final int RATE_60HZ = 16;
    /** 30Hz：115200 bps 低速链路下的省带宽档 */
    public static final int RATE_30HZ = 33;
    /** 不限速：每个触摸事件都发（BLE / 3Mbaud 时可以开） */
    public static final int RATE_OFF = 0;

    /** HID 鼠标报告里 X/Y/Wheel 都是 int8 */
    private static final int AXIS_MAX = 127;

    private final Sink sink;
    private final Clock clock;

    private int intervalMs = RATE_60HZ;

    /** 当前按键位图（待发/已发共用，按钮变化必须立刻生效） */
    private int buttons;
    /** 上一次真正发出去的按键位图 */
    private int sentButtons;

    private int pendingDx;
    private int pendingDy;
    private int pendingWheel;

    private boolean hasSent;
    private long lastSendAt;
    private Runnable pendingFlush;

    public PointerPump(Sink sink, Clock clock) {
        this.sink = sink;
        this.clock = clock;
        if (sink == null || clock == null) {
            throw new IllegalArgumentException("sink/clock 不能为 null");
        }
    }

    /** 0 = 不限速；16 = 60Hz；33 = 30Hz */
    public void setIntervalMs(int ms) {
        this.intervalMs = ms < 0 ? 0 : ms;
    }

    public int getIntervalMs() {
        return intervalMs;
    }

    /**
     * UI 线程每次触摸事件调用一次。
     *
     * @param buttons 当前按住的键位图
     * @param dx      相对位移（已由触控板换算成整数像素）
     * @param dy      相对位移
     * @param wheel   滚轮格数
     */
    public void onEvent(int buttons, int dx, int dy, int wheel) {
        long now = clock.now();
        int newButtons = buttons & 0x07;
        boolean buttonsChanged = newButtons != this.buttons;
        this.buttons = newButtons;

        if (dx != 0 || dy != 0 || wheel != 0) {
            pendingDx += dx;
            pendingDy += dy;
            pendingWheel += wheel;
        }

        // 按键变化、滚轮：立即发，绝不排队
        if (buttonsChanged || wheel != 0) {
            flush(now);
            return;
        }

        if (pendingDx == 0 && pendingDy == 0) {
            if (newButtons == sentButtons) {
                return; // 没有可发的东西
            }
            flush(now); // 例如全零的上报
            return;
        }

        if (intervalMs <= 0) {
            flush(now);
            return;
        }

        long due = hasSent ? lastSendAt + intervalMs : now;
        if (now >= due) {
            flush(now);
        } else {
            schedule(due - now);
        }
    }

    /** 把待发位移立刻送出去（切模式、暂停、松手时调用） */
    public void flush() {
        flush(clock.now());
    }

    /** 丢弃待发位移（不发送），用于关闭界面等场景 */
    public void reset() {
        pendingDx = 0;
        pendingDy = 0;
        pendingWheel = 0;
        cancelPending();
    }

    private void schedule(long delayMs) {
        if (pendingFlush != null) {
            return; // 已经排上了，不重复挂
        }
        pendingFlush = new Runnable() {
            @Override
            public void run() {
                pendingFlush = null;
                flush(clock.now());
            }
        };
        clock.postDelayed(pendingFlush, Math.max(1L, delayMs));
    }

    private void cancelPending() {
        if (pendingFlush != null) {
            clock.cancel(pendingFlush);
            pendingFlush = null;
        }
    }

    private void flush(long now) {
        cancelPending();
        int dx = clamp(pendingDx);
        int dy = clamp(pendingDy);
        int wheel = clamp(pendingWheel);
        boolean nothing = dx == 0 && dy == 0 && wheel == 0 && buttons == sentButtons;
        if (nothing) {
            return;
        }
        pendingDx = 0;
        pendingDy = 0;
        pendingWheel = 0;
        sentButtons = buttons;
        hasSent = true;
        lastSendAt = now;
        sink.onMouse(buttons, dx, dy, wheel);
    }

    private static int clamp(int v) {
        if (v > AXIS_MAX) {
            return AXIS_MAX;
        }
        return v < -AXIS_MAX ? -AXIS_MAX : v;
    }
}
