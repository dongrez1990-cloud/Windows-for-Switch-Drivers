package com.paopao.switchkey;

import java.util.ArrayList;
import java.util.List;

/**
 * 纯 Java 的按键状态机（不含任何 Android 依赖，方便用 JVM 直接跑测试）。
 *
 * 修饰键规则：
 *  · 按住 + 另一指按普通键         → 组合键（Shift+A / Ctrl+C）
 *  · 轻点一下（中间没按别的键）    → 一次性（one-shot），作用于下一个键后自动释放
 *  · 400ms 内连点两次              → 锁定，再点一次解除
 */
public final class KeyState {

    public interface Sink {
        void onReport(int modifiers, int[] usages, int count);
    }

    public static final int MAX_KEYS = 6;
    private static final long DOUBLE_TAP_MS = 400L;

    private final List<Integer> heldKeys = new ArrayList<>();
    private int heldMods;
    private int stickyMods;
    private int lockedMods;
    private boolean clearStickyPending;
    private boolean usedWhileHoldingMod;
    private final long[] lastModTap = new long[8];

    private long fixedNow = -1L;

    private Sink sink;

    public void setSink(Sink s) {
        this.sink = s;
    }

    /** 测试用：固定时钟 */
    public void setNow(long ms) {
        fixedNow = ms;
    }

    private long now() {
        return fixedNow >= 0 ? fixedNow : System.nanoTime() / 1000000L;
    }

    public int activeMods() {
        return heldMods | stickyMods | lockedMods;
    }

    public boolean isModActive(int modBit) {
        return (activeMods() & modBit) != 0;
    }

    public boolean isHeld(int usage) {
        return heldKeys.contains(usage);
    }

    public int heldCount() {
        return heldKeys.size();
    }

    public void keyDown(int usage, int modBit) {
        if (modBit != 0) {
            if ((lockedMods & modBit) != 0) {
                // 已锁定 → 再点一次解除
                lockedMods &= ~modBit;
                stickyMods &= ~modBit;
                usedWhileHoldingMod = false;
                emit();
                return;
            }
            heldMods |= modBit;
            usedWhileHoldingMod = false;
            emit();
            return;
        }
        if (heldKeys.contains(usage) || heldKeys.size() >= MAX_KEYS) {
            return;
        }
        heldKeys.add(usage);
        if (heldMods != 0) {
            usedWhileHoldingMod = true;
        }
        if (stickyMods != 0) {
            clearStickyPending = true;
        }
        emit();
    }

    public void keyUp(int usage, int modBit) {
        if (modBit != 0) {
            if ((heldMods & modBit) == 0) {
                return;
            }
            heldMods &= ~modBit;
            if (!usedWhileHoldingMod) {
                long t = now();
                int idx = Integer.numberOfTrailingZeros(modBit) & 7;
                boolean doubleTap = (t - lastModTap[idx]) < DOUBLE_TAP_MS;
                lastModTap[idx] = t;
                if (doubleTap) {
                    lockedMods |= modBit;
                    stickyMods &= ~modBit;
                } else if ((stickyMods & modBit) != 0) {
                    stickyMods &= ~modBit;
                } else {
                    stickyMods |= modBit;
                }
            }
            usedWhileHoldingMod = false;
            emit();
            return;
        }
        if (heldKeys.remove(Integer.valueOf(usage))) {
            if (heldKeys.isEmpty() && clearStickyPending) {
                clearStickyPending = false;
                stickyMods = 0;
            }
            emit();
        }
    }

    public void releaseAll() {
        heldKeys.clear();
        heldMods = 0;
        stickyMods = 0;
        lockedMods = 0;
        clearStickyPending = false;
        usedWhileHoldingMod = false;
        emit();
    }

    private void emit() {
        int[] usages = new int[MAX_KEYS];
        int n = 0;
        for (int u : heldKeys) {
            if (n >= MAX_KEYS) {
                break;
            }
            usages[n++] = u;
        }
        if (sink != null) {
            sink.onReport(activeMods(), usages, n);
        }
    }
}
