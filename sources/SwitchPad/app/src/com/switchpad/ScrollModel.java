package com.switchpad;

/**
 * 滚轮模型：两指在触控板上滑动的累计距离换算成 HID 滚轮“棘轮”（detent/notch）步数。
 *
 * Windows 默认一个棘轮滚动 3 行，所以这里必须做量化累加，
 * 否则每帧都发 ±1 会导致滚动速度失控。
 */
public final class ScrollModel {

    /** 默认每个棘轮对应的手指位移（dp），会按屏幕密度缩放 */
    public static final float DEFAULT_NOTCH_DP = 46f;

    private float notchPx = 46f * 3f; // 兜底值；实际由 UI 按密度设置
    private float gain = 1.0f;
    private boolean natural = false;
    private float acc;

    /** 单次报告最多发出的棘轮数，避免一次爆冲 */
    private static final int MAX_STEPS = 6;

    public void setNotchPx(float px) {
        if (px < 8f) px = 8f;
        this.notchPx = px;
    }

    public void setGain(float g) {
        if (g < 0.1f) g = 0.1f;
        if (g > 4f) g = 4f;
        this.gain = g;
    }

    public void setNatural(boolean on) {
        this.natural = on;
    }

    public boolean natural() {
        return natural;
    }

    public void reset() {
        acc = 0f;
    }

    /**
     * @param dy 两指质心的纵向位移（像素，下为正）
     * @return 本次应发送的滚轮步数；正数 = 向上滚（内容上移），负数 = 向下滚
     */
    public int offer(float dy) {
        // 自然滚动：手指下移 -> 内容下移 -> 滚轮负向
        float v = (natural ? -dy : dy) * gain / notchPx;
        acc += v;
        int steps = 0;
        while (acc >= 1f && steps < MAX_STEPS) {
            acc -= 1f;
            steps++;
        }
        while (acc <= -1f && steps > -MAX_STEPS) {
            acc += 1f;
            steps--;
        }
        if (steps >= MAX_STEPS || steps <= -MAX_STEPS) {
            acc = 0f; // 爆冲后清零，避免长期偏移
        }
        return steps;
    }
}
