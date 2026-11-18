package com.switchpad;

import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

/** 极简 UI 工具（纯 framework，无 AndroidX / Material 依赖）。 */
public final class Ui {

    public static final int BG = 0xFF10141A;
    public static final int PANEL = 0xFF181E27;
    public static final int PANEL_2 = 0xFF222A36;
    public static final int STROKE = 0xFF2E3947;
    public static final int TEXT = 0xFFE7EEF7;
    public static final int TEXT_DIM = 0xFF8C99AB;
    public static final int ACCENT = 0xFF3BA7FF;
    public static final int OK = 0xFF4ED08A;
    public static final int WARN = 0xFFF2B33D;
    public static final int ERR = 0xFFF2685C;

    private Ui() {
    }

    public static int dp(Context c, float v) {
        return Math.round(v * c.getResources().getDisplayMetrics().density);
    }

    public static GradientDrawable rounded(Context c, int color, int radiusDp, int strokeColor, int strokeDp) {
        GradientDrawable d = new GradientDrawable();
        d.setShape(GradientDrawable.RECTANGLE);
        d.setColor(color);
        d.setCornerRadius(dp(c, radiusDp));
        if (strokeDp > 0) d.setStroke(dp(c, strokeDp), strokeColor);
        return d;
    }

    public static Button button(Context c, String text, int bgColor, int textColor, float textSp) {
        Button b = new Button(c);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextColor(textColor);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, textSp);
        b.setBackground(rounded(c, bgColor, 10, STROKE, 1));
        b.setPadding(dp(c, 12), dp(c, 6), dp(c, 12), dp(c, 6));
        b.setMinHeight(0);
        b.setMinimumHeight(0);
        b.setMinWidth(0);
        b.setMinimumWidth(0);
        b.setStateListAnimator(null);
        b.setIncludeFontPadding(false);
        return b;
    }

    public static TextView text(Context c, String text, int color, float sp) {
        TextView t = new TextView(c);
        t.setText(text);
        t.setTextColor(color);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setIncludeFontPadding(false);
        return t;
    }

    public static LinearLayout row(Context c, int heightDp, int gravity) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setGravity(gravity);
        if (heightDp > 0) {
            l.setLayoutParams(new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, dp(c, heightDp)));
        }
        return l;
    }

    public static LinearLayout.LayoutParams lp(int w, int h) {
        return new LinearLayout.LayoutParams(w, h);
    }

    public static LinearLayout.LayoutParams wrap() {
        return new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    public static LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    public static void setVisible(View v, boolean visible) {
        v.setVisibility(visible ? View.VISIBLE : View.GONE);
    }

    public static int alpha(int color, float a) {
        return Color.argb((int) (255 * Math.max(0f, Math.min(1f, a))),
                Color.red(color), Color.green(color), Color.blue(color));
    }
}
