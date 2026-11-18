package com.paopao.switchkey;

import java.util.ArrayList;
import java.util.List;

/**
 * 键盘布局定义。长度单位是「1/12 屏宽」，每行宽度合计正好 12。
 * usage 是 USB HID Keyboard/Keypad 的使用码（Usage ID）。
 */
public final class KeyboardLayout {

    public static final int STYLE_NORMAL = 0;
    public static final int STYLE_SPECIAL = 1;
    public static final int STYLE_ACCENT = 2;

    // ---- HID usage IDs（USB HID Usage Tables, Keyboard/Keypad page）----
    public static final int U_ESC = 0x29;
    public static final int U_TAB = 0x2B;
    public static final int U_ENTER = 0x28;
    public static final int U_SPACE = 0x2C;
    public static final int U_BKSP = 0x2A;
    public static final int U_DEL = 0x4C;
    public static final int U_INSERT = 0x49;
    public static final int U_HOME = 0x4A;
    public static final int U_END = 0x4D;
    public static final int U_PGUP = 0x4B;
    public static final int U_PGDN = 0x4E;
    public static final int U_LEFT = 0x50;
    public static final int U_RIGHT = 0x4F;
    public static final int U_UP = 0x52;
    public static final int U_DOWN = 0x51;
    public static final int U_MINUS = 0x2D;
    public static final int U_EQUAL = 0x2E;
    public static final int U_LBRACKET = 0x2F;
    public static final int U_RBRACKET = 0x30;
    public static final int U_BACKSLASH = 0x31;
    public static final int U_SEMICOLON = 0x33;
    public static final int U_QUOTE = 0x34;
    public static final int U_COMMA = 0x36;
    public static final int U_PERIOD = 0x37;
    public static final int U_SLASH = 0x38;

    public static final class Key {
        public final String label;
        /** 第二标签（Shift 后的字符），没有就为 null */
        public final String sub;
        /** 非修饰键的 HID usage，修饰键为 0 */
        public final int usage;
        /** 修饰键位图（HidKeyboard.MOD_*），普通键为 0 */
        public final int mod;
        /** 宽度，单位 1/12 屏宽 */
        public final float width;
        public final int style;

        Key(String label, String sub, int usage, int mod, float width, int style) {
            this.label = label;
            this.sub = sub;
            this.usage = usage;
            this.mod = mod;
            this.width = width;
            this.style = style;
        }

        public boolean isModifier() {
            return mod != 0;
        }
    }

    private static Key key(String label, String sub, int usage, float width) {
        return new Key(label, sub, usage, 0, width, STYLE_NORMAL);
    }

    private static Key letter(int usage, String lower) {
        return new Key(lower, null, usage, 0, 0.9f, STYLE_NORMAL);
    }

    private static Key sp(String label, int usage, float width) {
        return new Key(label, null, usage, 0, width, STYLE_SPECIAL);
    }

    private static Key mod(String label, int modBit, float width) {
        return new Key(label, null, 0, modBit, width, STYLE_ACCENT);
    }

    /** 主键盘（字母区） */
    public static List<Key[]> mainRows() {
        List<Key[]> rows = new ArrayList<>();

        rows.add(new Key[] {
                sp("Esc", U_ESC, 1.2f),
                sp("Del", U_DEL, 1.2f),
                sp("Home", U_HOME, 1.2f),
                sp("End", U_END, 1.2f),
                sp("PgUp", U_PGUP, 1.2f),
                sp("PgDn", U_PGDN, 1.2f),
                sp("←", U_LEFT, 1.2f),
                sp("↑", U_UP, 1.2f),
                sp("↓", U_DOWN, 1.2f),
                sp("→", U_RIGHT, 1.2f),
        });

        rows.add(new Key[] {
                sp("Tab", U_TAB, 1.2f),
                key("1", "!", 0x1E, 0.9f),
                key("2", "@", 0x1F, 0.9f),
                key("3", "#", 0x20, 0.9f),
                key("4", "$", 0x21, 0.9f),
                key("5", "%", 0x22, 0.9f),
                key("6", "^", 0x23, 0.9f),
                key("7", "&", 0x24, 0.9f),
                key("8", "*", 0x25, 0.9f),
                key("9", "(", 0x26, 0.9f),
                key("0", ")", 0x27, 0.9f),
                key("-", "_", U_MINUS, 0.9f),
                key("=", "+", U_EQUAL, 0.9f),
        });

        rows.add(new Key[] {
                mod("Ctrl", HidKeyboard.MOD_LCTRL, 1.2f),
                letter(0x14, "q"),
                letter(0x1A, "w"),
                letter(0x08, "e"),
                letter(0x15, "r"),
                letter(0x17, "t"),
                letter(0x1C, "y"),
                letter(0x18, "u"),
                letter(0x0C, "i"),
                letter(0x12, "o"),
                letter(0x13, "p"),
                sp("⌫", U_BKSP, 1.8f),
        });

        rows.add(new Key[] {
                mod("⇧", HidKeyboard.MOD_LSHIFT, 1.0f),
                letter(0x04, "a"),
                letter(0x16, "s"),
                letter(0x07, "d"),
                letter(0x09, "f"),
                letter(0x0A, "g"),
                letter(0x0B, "h"),
                letter(0x0D, "j"),
                letter(0x0E, "k"),
                letter(0x0F, "l"),
                key(";", ":", U_SEMICOLON, 0.9f),
                key("'", "\"", U_QUOTE, 0.9f),
                sp("⏎", U_ENTER, 1.1f),
        });

        rows.add(new Key[] {
                mod("⇧", HidKeyboard.MOD_LSHIFT, 1.8f),
                letter(0x1D, "z"),
                letter(0x1B, "x"),
                letter(0x06, "c"),
                letter(0x19, "v"),
                letter(0x05, "b"),
                letter(0x11, "n"),
                letter(0x10, "m"),
                key(",", "<", U_COMMA, 0.9f),
                key(".", ">", U_PERIOD, 0.9f),
                key("/", "?", U_SLASH, 0.9f),
                key("\\", "|", U_BACKSLASH, 1.2f),
        });

        rows.add(new Key[] {
                mod("Win", HidKeyboard.MOD_LGUI, 1.2f),
                mod("Ctrl", HidKeyboard.MOD_LCTRL, 1.2f),
                mod("Alt", HidKeyboard.MOD_LALT, 1.2f),
                sp("", U_SPACE, 6.0f),
                key("[", "{", U_LBRACKET, 1.2f),
                key("]", "}", U_RBRACKET, 1.2f),
        });

        return rows;
    }
}
