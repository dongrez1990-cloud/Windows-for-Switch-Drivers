package com.switchpad;

import android.content.Context;
import android.content.SharedPreferences;

/** 轻量偏好设置（不引入 AndroidX，Pure framework SharedPreferences）。 */
public final class Prefs {

    private static final String FILE = "switchpad";
    private static final String K_SENS = "sensitivity";
    private static final String K_ACCEL = "acceleration";
    private static final String K_SCROLL_GAIN = "scroll_gain";
    private static final String K_SCROLL_NOTCH_DP = "scroll_notch_dp";
    private static final String K_NATURAL = "natural_scroll";
    private static final String K_TAP_CLICK = "tap_to_click";
    private static final String K_HAPTICS = "haptics";
    private static final String K_FN_ROW = "fn_row";
    private static final String K_LAYOUT = "layout_mode";
    private static final String K_COMPACT_DESC = "compact_descriptor";
    private static final String K_LONG_DRAG = "long_press_drag";

    private Prefs() {
    }

    private static SharedPreferences sp(Context c) {
        return c.getApplicationContext().getSharedPreferences(FILE, Context.MODE_PRIVATE);
    }

    /* ---------------- 指针 ---------------- */

    public static float sensitivity(Context c) {
        return sp(c).getFloat(K_SENS, 0.45f);
    }

    public static void setSensitivity(Context c, float v) {
        if (v < 0.10f) v = 0.10f;
        if (v > 3.00f) v = 3.00f;
        sp(c).edit().putFloat(K_SENS, v).apply();
    }

    public static boolean acceleration(Context c) {
        return sp(c).getBoolean(K_ACCEL, true);
    }

    public static void setAcceleration(Context c, boolean v) {
        sp(c).edit().putBoolean(K_ACCEL, v).apply();
    }

    /* ---------------- 滚轮 ---------------- */

    public static float scrollGain(Context c) {
        return sp(c).getFloat(K_SCROLL_GAIN, 1.0f);
    }

    public static void setScrollGain(Context c, float v) {
        sp(c).edit().putFloat(K_SCROLL_GAIN, v).apply();
    }

    /** 一个滚轮棘轮对应的手指位移（dp）。 */
    public static float scrollNotchDp(Context c) {
        return sp(c).getFloat(K_SCROLL_NOTCH_DP, ScrollModel.DEFAULT_NOTCH_DP);
    }

    public static void setScrollNotchDp(Context c, float v) {
        sp(c).edit().putFloat(K_SCROLL_NOTCH_DP, v).apply();
    }

    public static float scrollNotchPx(Context c) {
        return scrollNotchDp(c) * c.getResources().getDisplayMetrics().density;
    }

    public static boolean naturalScroll(Context c) {
        return sp(c).getBoolean(K_NATURAL, true);
    }

    public static void setNaturalScroll(Context c, boolean v) {
        sp(c).edit().putBoolean(K_NATURAL, v).apply();
    }

    /* ---------------- 手势 ---------------- */

    public static boolean tapToClick(Context c) {
        return sp(c).getBoolean(K_TAP_CLICK, true);
    }

    public static void setTapToClick(Context c, boolean v) {
        sp(c).edit().putBoolean(K_TAP_CLICK, v).apply();
    }

    public static boolean haptics(Context c) {
        return sp(c).getBoolean(K_HAPTICS, true);
    }

    public static void setHaptics(Context c, boolean v) {
        sp(c).edit().putBoolean(K_HAPTICS, v).apply();
    }

    public static boolean longPressDrag(Context c) {
        return sp(c).getBoolean(K_LONG_DRAG, true);
    }

    public static void setLongPressDrag(Context c, boolean v) {
        sp(c).edit().putBoolean(K_LONG_DRAG, v).apply();
    }

    /* ---------------- 键盘 ---------------- */

    public static boolean fnRow(Context c) {
        return sp(c).getBoolean(K_FN_ROW, false);
    }

    public static void setFnRow(Context c, boolean v) {
        sp(c).edit().putBoolean(K_FN_ROW, v).apply();
    }

    /* ---------------- 界面布局：0 分屏 / 1 仅触控板 / 2 仅键盘 ---------------- */

    public static int layoutMode(Context c) {
        int v = sp(c).getInt(K_LAYOUT, 0);
        return (v < 0 || v > 2) ? 0 : v;
    }

    public static void setLayoutMode(Context c, int v) {
        sp(c).edit().putInt(K_LAYOUT, v).apply();
    }

    /* ---------------- 兼容模式：SDP 里只声明键盘（描述符 63 字节） ---------------- */

    public static boolean compactDescriptor(Context c) {
        return sp(c).getBoolean(K_COMPACT_DESC, false);
    }

    public static void setCompactDescriptor(Context c, boolean v) {
        sp(c).edit().putBoolean(K_COMPACT_DESC, v).apply();
    }
}
