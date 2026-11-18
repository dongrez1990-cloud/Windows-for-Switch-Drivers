package com.switchpad;

import java.util.LinkedHashSet;

/**
 * 启动协议兼容的键盘报告模型（8 字节）。
 *
 * <pre>
 * byte0 : 修饰键位（LCTRL..RGUI）
 * byte1 : 保留（恒 0）
 * byte2..7 : 同时按下的 6 个 usage（不足补 0）
 * </pre>
 *
 * 只保留“当前按下的键集合”，因此重复按下同一个键不会产生重复字节，
 * 松开任意键后整体重发一次报告，这正符合 Boot Keyboard 语义。
 */
public final class KeyboardReport {

    /** 键盘输入报告的 Report ID（Android HID Device 内置描述符约定为 1） */
    public static final int REPORT_ID = 1;
    /** 报告长度（字节） */
    public static final int REPORT_LEN = 8;
    /** 最多同时上报的按键数 */
    public static final int MAX_KEYS = 6;

    private int modifiers;
    private final LinkedHashSet<Integer> keys = new LinkedHashSet<Integer>(MAX_KEYS);

    public synchronized int modifiers() {
        return modifiers;
    }

    public synchronized void setModifier(int modIndex, boolean down) {
        if (modIndex < 0 || modIndex > 7) return;
        if (down) {
            modifiers |= HidUsages.MOD_BIT[modIndex];
        } else {
            modifiers &= ~HidUsages.MOD_BIT[modIndex];
        }
    }

    public synchronized boolean isModifierDown(int modIndex) {
        if (modIndex < 0 || modIndex > 7) return false;
        return (modifiers & HidUsages.MOD_BIT[modIndex]) != 0;
    }

    public synchronized void clearModifiers() {
        modifiers = 0;
    }

    /** @return true 表示状态有变化（需要重发报告） */
    public synchronized boolean keyDown(int usage) {
        if (usage == HidUsages.K_NONE || usage < 0) return false;
        if (keys.contains(usage)) return false;
        if (keys.size() >= MAX_KEYS) return false;
        keys.add(usage);
        return true;
    }

    /** @return true 表示该键确实处于按下状态，被移除 */
    public synchronized boolean keyUp(int usage) {
        return keys.remove(usage);
    }

    public synchronized boolean isKeyDown(int usage) {
        return keys.contains(usage);
    }

    public synchronized int keyCount() {
        return keys.size();
    }

    public synchronized void releaseAll() {
        keys.clear();
        modifiers = 0;
    }

    /** 是否处于“空”状态（用于判断能否省掉一次发送） */
    public synchronized boolean isEmpty() {
        return modifiers == 0 && keys.isEmpty();
    }

    /** 生成 8 字节输入报告。 */
    public synchronized byte[] toReport() {
        byte[] r = new byte[REPORT_LEN];
        r[0] = (byte) (modifiers & 0xFF);
        r[1] = 0;
        int i = 2;
        for (Integer u : keys) {
            if (i >= REPORT_LEN) break;
            r[i++] = (byte) (u & 0xFF);
        }
        return r;
    }

    @Override
    public synchronized String toString() {
        StringBuilder sb = new StringBuilder("KB[");
        for (int i = 0; i < 8; i++) {
            if ((modifiers & HidUsages.MOD_BIT[i]) != 0) sb.append(HidUsages.MOD_NAME[i]).append('+');
        }
        for (Integer u : keys) sb.append(String.format("0x%02X ", u));
        return sb.append(']').toString();
    }
}
