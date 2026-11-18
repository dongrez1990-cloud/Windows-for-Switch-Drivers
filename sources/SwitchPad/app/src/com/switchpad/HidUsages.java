package com.switchpad;

/**
 * HID 用法表（Usage Tables）常量：键盘/小键盘页 0x07、消费类页 0x0C，
 * 以及启动键盘报告里 8 个修饰键位（modifier bits）的索引。
 *
 * 本类不依赖 Android，可在桌面 JDK 上直接跑自测。
 */
public final class HidUsages {

    private HidUsages() {
    }

    /* ---------------- 修饰键位（报告第 0 字节的 8 个 bit） ---------------- */

    public static final int MOD_LCTRL = 0;
    public static final int MOD_LSHIFT = 1;
    public static final int MOD_LALT = 2;
    public static final int MOD_LGUI = 3;
    public static final int MOD_RCTRL = 4;
    public static final int MOD_RSHIFT = 5;
    public static final int MOD_RALT = 6;
    public static final int MOD_RGUI = 7;

    /** 位索引 -> 修饰键 bit 掩码 */
    public static final int[] MOD_BIT = {
            0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80
    };

    public static final String[] MOD_NAME = {
            "LCtrl", "LShift", "LAlt", "LWin", "RCtrl", "RShift", "RAlt", "RWin"
    };

    /* ---------------- 常用键（Keyboard/Keypad, 0x07） ---------------- */

    public static final int K_NONE = 0x00;

    public static final int K_ENTER = 0x28;
    public static final int K_ESC = 0x29;
    public static final int K_BACKSPACE = 0x2A;
    public static final int K_TAB = 0x2B;
    public static final int K_SPACE = 0x2C;
    public static final int K_MINUS = 0x2D;
    public static final int K_EQUAL = 0x2E;
    public static final int K_LBRACKET = 0x2F;
    public static final int K_RBRACKET = 0x30;
    public static final int K_BACKSLASH = 0x31;
    public static final int K_SEMICOLON = 0x33;
    public static final int K_APOSTROPHE = 0x34;
    public static final int K_GRAVE = 0x35;
    public static final int K_COMMA = 0x36;
    public static final int K_PERIOD = 0x37;
    public static final int K_SLASH = 0x38;
    public static final int K_CAPS_LOCK = 0x39;

    public static final int K_F1 = 0x3A;   // F1..F12 = 0x3A..0x45
    public static final int K_PRINT_SCREEN = 0x46;
    public static final int K_SCROLL_LOCK = 0x47;
    public static final int K_PAUSE = 0x48;
    public static final int K_INSERT = 0x49;
    public static final int K_HOME = 0x4A;
    public static final int K_PAGE_UP = 0x4B;
    public static final int K_DELETE = 0x4C;
    public static final int K_END = 0x4D;
    public static final int K_PAGE_DOWN = 0x4E;
    public static final int K_RIGHT = 0x4F;
    public static final int K_LEFT = 0x50;
    public static final int K_DOWN = 0x51;
    public static final int K_UP = 0x52;

    public static final int K_NUM_LOCK = 0x53;
    public static final int K_APP = 0x65;

    /* ---------------- 消费类（Consumer, 0x0C） ---------------- */

    public static final int C_VOLUME_UP = 0x00E9;
    public static final int C_VOLUME_DOWN = 0x00EA;
    public static final int C_MUTE = 0x00E2;
    public static final int C_PLAY_PAUSE = 0x00CD;
    public static final int C_NEXT_TRACK = 0x00B5;
    public static final int C_PREV_TRACK = 0x00B6;
    public static final int C_STOP = 0x00B7;
    public static final int C_BRIGHTNESS_UP = 0x006F;
    public static final int C_BRIGHTNESS_DOWN = 0x0070;

    /* ---------------- 鼠标按键掩码 ---------------- */

    public static final int BTN_LEFT = 0x01;
    public static final int BTN_RIGHT = 0x02;
    public static final int BTN_MIDDLE = 0x04;

    /* ---------------- 字符 -> usage ---------------- */

    /** 小写字母 a..z -> 0x04..0x1D，其余返回 0。 */
    public static int letter(char c) {
        if (c >= 'a' && c <= 'z') return 0x04 + (c - 'a');
        if (c >= 'A' && c <= 'Z') return 0x04 + (c - 'A');
        return 0;
    }

    /** 数字 '1'..'9' -> 0x1E..0x26，'0' -> 0x27，其余返回 0。 */
    public static int digit(char c) {
        if (c >= '1' && c <= '9') return 0x1D + (c - '0'); // '1' -> 0x1E
        if (c == '0') return 0x27;
        return 0;
    }

    /** 单个 ASCII 字符 -> 主键 usage（不含 shift 语义，符号取下档键位）。 */
    public static int key(char c) {
        int u = letter(c);
        if (u != 0) return u;
        u = digit(c);
        if (u != 0) return u;
        switch (c) {
            case ' ':
                return K_SPACE;
            case '-':
            case '_':
                return K_MINUS;
            case '=':
            case '+':
                return K_EQUAL;
            case '[':
            case '{':
                return K_LBRACKET;
            case ']':
            case '}':
                return K_RBRACKET;
            case '\\':
            case '|':
                return K_BACKSLASH;
            case ';':
            case ':':
                return K_SEMICOLON;
            case '\'':
            case '"':
                return K_APOSTROPHE;
            case '`':
            case '~':
                return K_GRAVE;
            case ',':
            case '<':
                return K_COMMA;
            case '.':
            case '>':
                return K_PERIOD;
            case '/':
            case '?':
                return K_SLASH;
            default:
                return 0;
        }
    }

    /** 该字符在美式键盘上是否需要按住 Shift。 */
    public static boolean needsShift(char c) {
        return c == '_' || c == '+' || c == '{' || c == '}' || c == '|' || c == ':'
                || c == '"' || c == '~' || c == '<' || c == '>' || c == '?'
                || (c >= 'A' && c <= 'Z')
                || "!@#$%^&*()".indexOf(c) >= 0;
    }
}
