package com.switchpad;

/**
 * 复合 HID 报告描述符：一个设备同时提供「键盘 + 鼠标 + 消费类按键」三套集合。
 *
 * Android 8.1/9 起系统自带的 HID Device（BluetoothHidDevice）实现里已经内置了
 * 一份等价描述符（键盘 = Report ID 1，鼠标 = Report ID 2，消费类 = Report ID 3），
 * 大多数 ROM 会忽略 App 传入的 descriptor 参数、直接使用内置的那份；这里仍然
 * 完整带上，一是给 Windows 侧的 SDP/HID 描述信息用，二是把它作为本文档与
 * 将来 BLE-GATT-HID 方案（可自定义 Report Map）的唯一权威来源。
 *
 * 三个集合的输入报告长度分别是 8 / 4 / 2 字节，见 {@link #KEYBOARD_REPORT_LEN} 等。
 */
public final class HidDescriptor {

    private HidDescriptor() {
    }

    public static final int REPORT_ID_KEYBOARD = 1;
    public static final int REPORT_ID_MOUSE = 2;
    public static final int REPORT_ID_CONSUMER = 3;

    public static final int KEYBOARD_REPORT_LEN = 8;
    public static final int MOUSE_REPORT_LEN = 4;
    public static final int CONSUMER_REPORT_LEN = 2;

    /** HID 子类：0xC0 = 键盘(0x40) | 定点设备(0x80)，组合键鼠 */
    public static final byte SUBCLASS_COMBO = (byte) 0xC0;
    /** HID 子类：纯键盘 */
    public static final byte SUBCLASS_KEYBOARD = (byte) 0x40;

    /** 键盘应用集合 + 鼠标应用集合 + 消费类应用集合 */
    private static final int[] RAW = {
            /* ---- 键盘：Report ID 1 ---- */
            0x05, 0x01,             // Usage Page (Generic Desktop)
            0x09, 0x06,             // Usage (Keyboard)
            0xA1, 0x01,             // Collection (Application)
            0x85, 0x01,             //   Report ID (1)
            0x05, 0x07,             //   Usage Page (Keyboard/Keypad)
            0x19, 0xE0,             //   Usage Minimum (LeftControl)
            0x29, 0xE7,             //   Usage Maximum (Right GUI)
            0x15, 0x00,             //   Logical Minimum (0)
            0x25, 0x01,             //   Logical Maximum (1)
            0x75, 0x01,             //   Report Size (1)
            0x95, 0x08,             //   Report Count (8)
            0x81, 0x02,             //   Input (Data,Var,Abs)          -> 修饰键字节
            0x95, 0x01,             //   Report Count (1)
            0x75, 0x08,             //   Report Size (8)
            0x81, 0x01,             //   Input (Const,Array,Abs)       -> 保留字节
            0x95, 0x05,             //   Report Count (5)
            0x75, 0x01,             //   Report Size (1)
            0x05, 0x08,             //   Usage Page (LEDs)
            0x19, 0x01,             //   Usage Minimum (Num Lock)
            0x29, 0x05,             //   Usage Maximum (Kana)
            0x91, 0x02,             //   Output (Data,Var,Abs)         -> LEDs
            0x95, 0x01,             //   Report Count (1)
            0x75, 0x03,             //   Report Size (3)
            0x91, 0x01,             //   Output (Const,Array,Abs)      -> LED 补位
            0x95, 0x06,             //   Report Count (6)
            0x75, 0x08,             //   Report Size (8)
            0x15, 0x00,             //   Logical Minimum (0)
            0x25, 0x65,             //   Logical Maximum (101)
            0x05, 0x07,             //   Usage Page (Keyboard/Keypad)
            0x19, 0x00,             //   Usage Minimum (0)
            0x29, 0x65,             //   Usage Maximum (101)
            0x81, 0x00,             //   Input (Data,Array,Abs)        -> 6 个按键槽
            0xC0,                   // End Collection

            /* ---- 鼠标：Report ID 2（相对坐标 + 滚轮） ---- */
            0x05, 0x01,             // Usage Page (Generic Desktop)
            0x09, 0x02,             // Usage (Mouse)
            0xA1, 0x01,             // Collection (Application)
            0x09, 0x01,             //   Usage (Pointer)
            0xA1, 0x00,             //   Collection (Physical)
            0x85, 0x02,             //     Report ID (2)
            0x05, 0x09,             //     Usage Page (Button)
            0x19, 0x01,             //     Usage Minimum (1)
            0x29, 0x03,             //     Usage Maximum (3)
            0x15, 0x00,             //     Logical Minimum (0)
            0x25, 0x01,             //     Logical Maximum (1)
            0x95, 0x03,             //     Report Count (3)
            0x75, 0x01,             //     Report Size (1)
            0x81, 0x02,             //     Input (Data,Var,Abs)        -> 左/右/中键
            0x95, 0x01,             //     Report Count (1)
            0x75, 0x05,             //     Report Size (5)
            0x81, 0x01,             //     Input (Const,Array,Abs)     -> 补位
            0x05, 0x01,             //     Usage Page (Generic Desktop)
            0x09, 0x30,             //     Usage (X)
            0x09, 0x31,             //     Usage (Y)
            0x09, 0x38,             //     Usage (Wheel)
            0x15, 0x81,             //     Logical Minimum (-127)
            0x25, 0x7F,             //     Logical Maximum (127)
            0x75, 0x08,             //     Report Size (8)
            0x95, 0x03,             //     Report Count (3)
            0x81, 0x06,             //     Input (Data,Var,Rel)
            0xC0,                   //   End Collection (Physical)
            0xC0,                   // End Collection (Application)

            /* ---- 消费类按键：Report ID 3（音量/媒体/亮度） ---- */
            0x05, 0x0C,             // Usage Page (Consumer)
            0x09, 0x01,             // Usage (Consumer Control)
            0xA1, 0x01,             // Collection (Application)
            0x85, 0x03,             //   Report ID (3)
            0x15, 0x00,             //   Logical Minimum (0)
            0x26, 0xFF, 0x03,       //   Logical Maximum (1023)
            0x19, 0x00,             //   Usage Minimum (0)
            0x2A, 0xFF, 0x03,       //   Usage Maximum (1023)
            0x75, 0x10,             //   Report Size (16)
            0x95, 0x01,             //   Report Count (1)
            0x81, 0x00,             //   Input (Data,Array,Abs)
            0xC0                    // End Collection
    };

    /**
     * 精简版：只声明一个启动协议兼容的键盘（Report ID 1，63 字节）。
     * 有的主机蓝牙栈对 SDP 里 HIDDescriptorList 的大小/形状很挑（SDP 响应要经
     * L2CAP 分片 + ACL 重组），这时换成这份最容易过的描述符能验证链路本身。
     */
    private static final int[] RAW_KEYBOARD_ONLY = {
            0x05, 0x01,             // Usage Page (Generic Desktop)
            0x09, 0x06,             // Usage (Keyboard)
            0xA1, 0x01,             // Collection (Application)
            0x85, 0x01,             //   Report ID (1)
            0x05, 0x07,             //   Usage Page (Keyboard/Keypad)
            0x19, 0xE0,             //   Usage Minimum (LeftControl)
            0x29, 0xE7,             //   Usage Maximum (Right GUI)
            0x15, 0x00,             //   Logical Minimum (0)
            0x25, 0x01,             //   Logical Maximum (1)
            0x75, 0x01,             //   Report Size (1)
            0x95, 0x08,             //   Report Count (8)
            0x81, 0x02,             //   Input (Data,Var,Abs)
            0x95, 0x01,             //   Report Count (1)
            0x75, 0x08,             //   Report Size (8)
            0x81, 0x01,             //   Input (Const)
            0x95, 0x05,             //   Report Count (5)
            0x75, 0x01,             //   Report Size (1)
            0x05, 0x08,             //   Usage Page (LEDs)
            0x19, 0x01,             //   Usage Minimum (Num Lock)
            0x29, 0x05,             //   Usage Maximum (Kana)
            0x91, 0x02,             //   Output (Data,Var,Abs)
            0x95, 0x01,             //   Report Count (1)
            0x75, 0x03,             //   Report Size (3)
            0x91, 0x01,             //   Output (Const)
            0x95, 0x06,             //   Report Count (6)
            0x75, 0x08,             //   Report Size (8)
            0x15, 0x00,             //   Logical Minimum (0)
            0x25, 0x65,             //   Logical Maximum (101)
            0x05, 0x07,             //   Usage Page (Keyboard/Keypad)
            0x19, 0x00,             //   Usage Minimum (0)
            0x29, 0x65,             //   Usage Maximum (101)
            0x81, 0x00,             //   Input (Data,Array,Abs)
            0xC0                    // End Collection
    };

    private static final byte[] DESCRIPTOR = toBytes(RAW);
    private static final byte[] DESCRIPTOR_KEYBOARD_ONLY = toBytes(RAW_KEYBOARD_ONLY);

    /** 返回复合报告描述符的副本（调用方可随意修改）。 */
    public static byte[] reportDescriptor() {
        return copy(DESCRIPTOR);
    }

    /** 仅键盘的精简描述符 */
    public static byte[] keyboardOnlyDescriptor() {
        return copy(DESCRIPTOR_KEYBOARD_ONLY);
    }

    /** 按"兼容模式"取描述符：compact=true 时只声明键盘 */
    public static byte[] reportDescriptor(boolean compact) {
        return compact ? keyboardOnlyDescriptor() : reportDescriptor();
    }

    public static int length(boolean compact) {
        return compact ? DESCRIPTOR_KEYBOARD_ONLY.length : DESCRIPTOR.length;
    }

    private static byte[] copy(byte[] src) {
        byte[] copy = new byte[src.length];
        System.arraycopy(src, 0, copy, 0, src.length);
        return copy;
    }

    public static int length() {
        return DESCRIPTOR.length;
    }

    private static byte[] toBytes(int[] src) {
        byte[] out = new byte[src.length];
        for (int i = 0; i < src.length; i++) out[i] = (byte) (src[i] & 0xFF);
        return out;
    }
}
