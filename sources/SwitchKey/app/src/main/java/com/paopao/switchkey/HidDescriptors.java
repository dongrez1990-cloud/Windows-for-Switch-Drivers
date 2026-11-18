package com.paopao.switchkey;

/**
 * HID 报告描述符（纯 Java，不含 Android 依赖，方便用 JVM 解析测试）。
 *
 * 复合设备：
 *   Report ID 1 = 键盘，8 字节负载（修饰键 / 保留 / 6 个 usage）
 *   Report ID 2 = 鼠标，4 字节负载（按键位 / dx / dy / 滚轮，相对位移）
 *
 * 一个蓝牙连接同时提供键盘和鼠标，出门就不用再带实体鼠标去抢蓝牙链路了。
 */
public final class HidDescriptors {

    public static final int REPORT_ID_KEYBOARD = 1;
    public static final int REPORT_ID_MOUSE = 2;

    /** 键盘输入报告负载长度（不含 report id） */
    public static final int KEYBOARD_PAYLOAD = 8;
    /** 鼠标输入报告负载长度（不含 report id） */
    public static final int MOUSE_PAYLOAD = 4;

    private HidDescriptors() {
    }

    /**
     * 兼容模式：只用键盘，且**不使用 report id**（最简单的单 collection 描述符）。
     * 有些主机的 HID 驱动对复合设备不友好，出问题时切到这个模式重新配对。
     */
    public static final byte[] REPORT_MAP_KEYBOARD_ONLY = new byte[] {
            (byte) 0x05, (byte) 0x01,        // USAGE_PAGE (Generic Desktop)
            (byte) 0x09, (byte) 0x06,        // USAGE (Keyboard)
            (byte) 0xA1, (byte) 0x01,        // COLLECTION (Application)
            (byte) 0x05, (byte) 0x07,        //   USAGE_PAGE (Keyboard)
            (byte) 0x19, (byte) 0xE0,        //   USAGE_MINIMUM (Left Control)
            (byte) 0x29, (byte) 0xE7,        //   USAGE_MAXIMUM (Right GUI)
            (byte) 0x15, (byte) 0x00,        //   LOGICAL_MINIMUM (0)
            (byte) 0x25, (byte) 0x01,        //   LOGICAL_MAXIMUM (1)
            (byte) 0x95, (byte) 0x08,        //   REPORT_COUNT (8)
            (byte) 0x75, (byte) 0x01,        //   REPORT_SIZE (1)
            (byte) 0x81, (byte) 0x02,        //   INPUT (Data,Var,Abs)  修饰键位图
            (byte) 0x95, (byte) 0x01,        //   REPORT_COUNT (1)
            (byte) 0x75, (byte) 0x08,        //   REPORT_SIZE (8)
            (byte) 0x81, (byte) 0x01,        //   INPUT (Const)         保留字节
            (byte) 0x05, (byte) 0x08,        //   USAGE_PAGE (LEDs)
            (byte) 0x19, (byte) 0x01,        //   USAGE_MINIMUM (Num Lock)
            (byte) 0x29, (byte) 0x05,        //   USAGE_MAXIMUM (Kana)
            (byte) 0x95, (byte) 0x05,        //   REPORT_COUNT (5)
            (byte) 0x75, (byte) 0x01,        //   REPORT_SIZE (1)
            (byte) 0x91, (byte) 0x02,        //   OUTPUT (Data,Var,Abs) LED
            (byte) 0x95, (byte) 0x01,        //   REPORT_COUNT (1)
            (byte) 0x75, (byte) 0x03,        //   REPORT_SIZE (3)
            (byte) 0x91, (byte) 0x01,        //   OUTPUT (Const)        补齐
            (byte) 0x05, (byte) 0x07,        //   USAGE_PAGE (Keyboard)
            (byte) 0x19, (byte) 0x00,        //   USAGE_MINIMUM (0)
            (byte) 0x29, (byte) 0x65,        //   USAGE_MAXIMUM (101)
            (byte) 0x15, (byte) 0x00,        //   LOGICAL_MINIMUM (0)
            (byte) 0x25, (byte) 0x65,        //   LOGICAL_MAXIMUM (101)
            (byte) 0x95, (byte) 0x06,        //   REPORT_COUNT (6)
            (byte) 0x75, (byte) 0x08,        //   REPORT_SIZE (8)
            (byte) 0x81, (byte) 0x00,        //   INPUT (Data,Array,Abs) 按键数组
            (byte) 0xC0                      // END_COLLECTION
    };

    public static final byte[] REPORT_MAP = new byte[] {
            // ---------------- 键盘 ----------------
            (byte) 0x05, (byte) 0x01,        // USAGE_PAGE (Generic Desktop)
            (byte) 0x09, (byte) 0x06,        // USAGE (Keyboard)
            (byte) 0xA1, (byte) 0x01,        // COLLECTION (Application)
            (byte) 0x85, (byte) 0x01,        //   REPORT_ID (1)
            (byte) 0x05, (byte) 0x07,        //   USAGE_PAGE (Keyboard)
            (byte) 0x19, (byte) 0xE0,        //   USAGE_MINIMUM (Left Control)
            (byte) 0x29, (byte) 0xE7,        //   USAGE_MAXIMUM (Right GUI)
            (byte) 0x15, (byte) 0x00,        //   LOGICAL_MINIMUM (0)
            (byte) 0x25, (byte) 0x01,        //   LOGICAL_MAXIMUM (1)
            (byte) 0x95, (byte) 0x08,        //   REPORT_COUNT (8)
            (byte) 0x75, (byte) 0x01,        //   REPORT_SIZE (1)
            (byte) 0x81, (byte) 0x02,        //   INPUT (Data,Var,Abs)  修饰键位图
            (byte) 0x95, (byte) 0x01,        //   REPORT_COUNT (1)
            (byte) 0x75, (byte) 0x08,        //   REPORT_SIZE (8)
            (byte) 0x81, (byte) 0x01,        //   INPUT (Const)         保留字节
            (byte) 0x05, (byte) 0x08,        //   USAGE_PAGE (LEDs)
            (byte) 0x19, (byte) 0x01,        //   USAGE_MINIMUM (Num Lock)
            (byte) 0x29, (byte) 0x05,        //   USAGE_MAXIMUM (Kana)
            (byte) 0x95, (byte) 0x05,        //   REPORT_COUNT (5)
            (byte) 0x75, (byte) 0x01,        //   REPORT_SIZE (1)
            (byte) 0x91, (byte) 0x02,        //   OUTPUT (Data,Var,Abs) LED
            (byte) 0x95, (byte) 0x01,        //   REPORT_COUNT (1)
            (byte) 0x75, (byte) 0x03,        //   REPORT_SIZE (3)
            (byte) 0x91, (byte) 0x01,        //   OUTPUT (Const)        补齐
            (byte) 0x05, (byte) 0x07,        //   USAGE_PAGE (Keyboard)
            (byte) 0x19, (byte) 0x00,        //   USAGE_MINIMUM (0)
            (byte) 0x29, (byte) 0x65,        //   USAGE_MAXIMUM (101)
            (byte) 0x15, (byte) 0x00,        //   LOGICAL_MINIMUM (0)
            (byte) 0x25, (byte) 0x65,        //   LOGICAL_MAXIMUM (101)
            (byte) 0x95, (byte) 0x06,        //   REPORT_COUNT (6)
            (byte) 0x75, (byte) 0x08,        //   REPORT_SIZE (8)
            (byte) 0x81, (byte) 0x00,        //   INPUT (Data,Array,Abs) 按键数组
            (byte) 0xC0,                     // END_COLLECTION

            // ---------------- 鼠标 ----------------
            (byte) 0x05, (byte) 0x01,        // USAGE_PAGE (Generic Desktop)
            (byte) 0x09, (byte) 0x02,        // USAGE (Mouse)
            (byte) 0xA1, (byte) 0x01,        // COLLECTION (Application)
            (byte) 0x85, (byte) 0x02,        //   REPORT_ID (2)
            (byte) 0x09, (byte) 0x01,        //   USAGE (Pointer)
            (byte) 0xA1, (byte) 0x00,        //   COLLECTION (Physical)
            (byte) 0x05, (byte) 0x09,        //     USAGE_PAGE (Button)
            (byte) 0x19, (byte) 0x01,        //     USAGE_MINIMUM (Button 1)
            (byte) 0x29, (byte) 0x03,        //     USAGE_MAXIMUM (Button 3)
            (byte) 0x15, (byte) 0x00,        //     LOGICAL_MINIMUM (0)
            (byte) 0x25, (byte) 0x01,        //     LOGICAL_MAXIMUM (1)
            (byte) 0x95, (byte) 0x03,        //     REPORT_COUNT (3)
            (byte) 0x75, (byte) 0x01,        //     REPORT_SIZE (1)
            (byte) 0x81, (byte) 0x02,        //     INPUT (Data,Var,Abs) 左中右
            (byte) 0x95, (byte) 0x01,        //     REPORT_COUNT (1)
            (byte) 0x75, (byte) 0x05,        //     REPORT_SIZE (5)
            (byte) 0x81, (byte) 0x01,        //     INPUT (Const)        补齐
            (byte) 0x05, (byte) 0x01,        //     USAGE_PAGE (Generic Desktop)
            (byte) 0x09, (byte) 0x30,        //     USAGE (X)
            (byte) 0x09, (byte) 0x31,        //     USAGE (Y)
            (byte) 0x15, (byte) 0x81,        //     LOGICAL_MINIMUM (-127)
            (byte) 0x25, (byte) 0x7F,        //     LOGICAL_MAXIMUM (127)
            (byte) 0x75, (byte) 0x08,        //     REPORT_SIZE (8)
            (byte) 0x95, (byte) 0x02,        //     REPORT_COUNT (2)
            (byte) 0x81, (byte) 0x06,        //     INPUT (Data,Var,Rel) 相对位移
            (byte) 0x09, (byte) 0x38,        //     USAGE (Wheel)
            (byte) 0x15, (byte) 0x81,        //     LOGICAL_MINIMUM (-127)
            (byte) 0x25, (byte) 0x7F,        //     LOGICAL_MAXIMUM (127)
            (byte) 0x75, (byte) 0x08,        //     REPORT_SIZE (8)
            (byte) 0x95, (byte) 0x01,        //     REPORT_COUNT (1)
            (byte) 0x81, (byte) 0x06,        //     INPUT (Data,Var,Rel) 滚轮
            (byte) 0xC0,                     //   END_COLLECTION (Physical)
            (byte) 0xC0                      // END_COLLECTION (Application)
    };
}
