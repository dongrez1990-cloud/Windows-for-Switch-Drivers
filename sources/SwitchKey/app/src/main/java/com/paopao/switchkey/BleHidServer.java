package com.paopao.switchkey;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattServer;
import android.bluetooth.BluetoothGattServerCallback;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.AdvertiseCallback;
import android.bluetooth.le.AdvertiseData;
import android.bluetooth.le.AdvertiseSettings;
import android.bluetooth.le.BluetoothLeAdvertiser;
import android.content.Context;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.util.Log;

import java.util.Locale;
import java.util.UUID;

/**
 * BLE HID over GATT（HOGP）实现：把手机做成 BLE 从设备，Windows 当 BLE 主机。
 *
 * 服务 0x1812，特征：
 *   0x2A4A HID Information      (read)
 *   0x2A4B Report Map           (read，标准键盘/鼠标报告描述符)
 *   0x2A4C HID Control Point    (write without response)
 *   0x2A4D Report               (read/write/notify —— 输入靠 notify 推)
 *   0x2A4E Protocol Mode        (read/write，默认 Report 模式)
 *   0x2A22 Boot Keyboard Input Report / 0x2A32 Boot Keyboard Output Report（Boot 模式用）
 *
 * 注意：Report 特征必须挂 Report Reference 描述符（0x2908），
 * 否则 Windows 无法把报告映射到具体 report id —— 这是规范表里最容易漏的一环。
 */
public final class BleHidServer {

    private static final String TAG = "SwitchKeyBle";

    public static final UUID UUID_HID_SERVICE = uuid16(0x1812);
    public static final UUID UUID_HID_INFO = uuid16(0x2A4A);
    public static final UUID UUID_REPORT_MAP = uuid16(0x2A4B);
    public static final UUID UUID_CONTROL_POINT = uuid16(0x2A4C);
    public static final UUID UUID_REPORT = uuid16(0x2A4D);
    public static final UUID UUID_PROTOCOL_MODE = uuid16(0x2A4E);
    public static final UUID UUID_BOOT_KB_IN = uuid16(0x2A22);
    public static final UUID UUID_BOOT_KB_OUT = uuid16(0x2A32);
    /** Report Reference descriptor */
    public static final UUID UUID_REPORT_REF = uuid16(0x2908);
    /** Client Characteristic Configuration */
    public static final UUID UUID_CCCD = uuid16(0x2902);

    private static final byte PROTOCOL_BOOT = 0x00;
    private static final byte PROTOCOL_REPORT = 0x01;

    private static final int REPORT_TYPE_INPUT = 0x01;
    private static final int REPORT_TYPE_OUTPUT = 0x02;
    private static final int REPORT_TYPE_FEATURE = 0x03;

    private static UUID uuid16(int value) {
        return UUID.fromString(String.format(Locale.US,
                "0000%04x-0000-1000-8000-00805F9B34FB", value));
    }

    public interface Listener {
        void onBleState(int state, String title, String detail);

        void onBleDiag(String line);
    }

    public static final int STATE_OFF = 0;
    public static final int STATE_ADVERTISING = 1;
    public static final int STATE_CONNECTED = 2;
    public static final int STATE_ERROR = 3;

    private final Context appContext;
    private final Handler main = new Handler(Looper.getMainLooper());

    private BluetoothAdapter adapter;
    private BluetoothGattServer server;
    private BluetoothLeAdvertiser advertiser;
    private BluetoothDevice host;
    private Listener listener;

    private boolean keyboardOnly;
    /** true = 自己给可通知特征补 CCCD(0x2902)；false = 依赖系统自动补 */
    private boolean manualCccd;
    private int state = STATE_OFF;
    private byte protocolMode = PROTOCOL_REPORT;

    private BluetoothGattCharacteristic reportMapCh;
    private BluetoothGattCharacteristic protocolModeCh;
    private BluetoothGattCharacteristic keyboardReportCh;
    private BluetoothGattCharacteristic mouseReportCh;
    private BluetoothGattCharacteristic bootKeyboardInCh;

    private volatile boolean keyboardNotifyEnabled;
    private volatile boolean mouseNotifyEnabled;
    private volatile boolean bootNotifyEnabled;

    private int lastMods;
    private final int[] lastKeys = new int[6];
    private int lastCount;
    private int mouseButtons;

    public BleHidServer(Context ctx) {
        this.appContext = ctx.getApplicationContext();
    }

    public void setListener(Listener l) {
        this.listener = l;
        if (l != null) {
            l.onBleState(state, titleOf(state), detailOf(state));
        }
    }

    public int getState() {
        return state;
    }

    public boolean isConnected() {
        return state == STATE_CONNECTED;
    }

    public String getHostLabel() {
        return host == null ? "" : label(host);
    }

    /** 踢掉当前连接（比如被手环/耳机之类的设备误连占位） */
    public void disconnectHost() {
        BluetoothGattServer s = server;
        BluetoothDevice d = host;
        if (s == null || d == null) {
            diag("当前没有 BLE 连接可断开");
            return;
        }
        diag("主动断开 BLE 连接: " + label(d));
        try {
            s.cancelConnection(d);
        } catch (Throwable t) {
            diag("cancelConnection 失败: " + t);
        }
    }

    public void setKeyboardOnly(boolean only) {
        this.keyboardOnly = only;
    }

    public boolean isManualCccd() {
        return manualCccd;
    }

    /** 切换「谁补 CCCD」，切换后需要重建 GATT 服务 */
    public void setManualCccd(boolean manual) {
        if (manualCccd == manual) {
            return;
        }
        manualCccd = manual;
        diag("CCCD 模式 -> " + (manual ? "手动补" : "依赖系统自动补"));
        if (server != null) {
            stop();
            start();
        }
    }

    private void addCccd(BluetoothGattCharacteristic ch) {
        BluetoothGattDescriptor cccd = new BluetoothGattDescriptor(
                UUID_CCCD,
                BluetoothGattDescriptor.PERMISSION_READ | BluetoothGattDescriptor.PERMISSION_WRITE);
        cccd.setValue(new byte[] { 0x00, 0x00 });
        ch.addDescriptor(cccd);
    }

    public String getAdapterName() {
        ensureAdapter();
        if (adapter == null) {
            return "?";
        }
        try {
            String n = adapter.getName();
            return n == null ? "?" : n;
        } catch (SecurityException e) {
            return "?";
        }
    }

    private void ensureAdapter() {
        if (adapter == null) {
            BluetoothManager bm = (BluetoothManager) appContext.getSystemService(Context.BLUETOOTH_SERVICE);
            if (bm != null) {
                adapter = bm.getAdapter();
            }
        }
    }

    // ---------------- 生命周期 ----------------

    public void start() {
        ensureAdapter();
        if (adapter == null) {
            setState(STATE_ERROR, "没有蓝牙适配器", "无法启动 BLE HID");
            return;
        }
        if (!adapter.isEnabled()) {
            setState(STATE_ERROR, "蓝牙未开启", "打开蓝牙后再试");
            return;
        }
        if (!adapter.isMultipleAdvertisementSupported()) {
            diag("注意：isMultipleAdvertisementSupported=false，仍尝试广播");
        }
        if (server != null) {
            startAdvertising();
            return;
        }
        diag("openGattServer…");
        try {
            server = ((BluetoothManager) appContext.getSystemService(Context.BLUETOOTH_SERVICE))
                    .openGattServer(appContext, serverCallback);
        } catch (Throwable t) {
            diag("openGattServer 失败: " + t);
            server = null;
        }
        if (server == null) {
            setState(STATE_ERROR, "无法创建 GATT 服务", "手机可能不支持 BLE 从设备模式");
            return;
        }
        BluetoothGattService svc = buildService();
        boolean added;
        try {
            added = server.addService(svc);
        } catch (Throwable t) {
            diag("addService 异常: " + t);
            added = false;
        }
        diag("addService(0x1812) 返回 " + added);
        if (!added) {
            setState(STATE_ERROR, "注册 HID GATT 服务失败", "系统拒绝了 0x1812 服务");
            return;
        }
        startAdvertising();
    }

    private BluetoothGattService buildService() {
        BluetoothGattService svc = new BluetoothGattService(
                UUID_HID_SERVICE, BluetoothGattService.SERVICE_TYPE_PRIMARY);

        // 0x2A4A HID Information: bcdHID + countryCode + flags
        BluetoothGattCharacteristic info = new BluetoothGattCharacteristic(
                UUID_HID_INFO,
                BluetoothGattCharacteristic.PROPERTY_READ,
                BluetoothGattDescriptor.PERMISSION_READ);
        info.setValue(new byte[] { (byte) 0x11, (byte) 0x01, (byte) 0x00, (byte) 0x02 });
        svc.addCharacteristic(info);

        // 0x2A4B Report Map
        reportMapCh = new BluetoothGattCharacteristic(
                UUID_REPORT_MAP,
                BluetoothGattCharacteristic.PROPERTY_READ,
                BluetoothGattDescriptor.PERMISSION_READ);
        reportMapCh.setValue(keyboardOnly
                ? HidDescriptors.REPORT_MAP_KEYBOARD_ONLY
                : HidDescriptors.REPORT_MAP);
        svc.addCharacteristic(reportMapCh);

        // 0x2A4C HID Control Point（挂起/恢复，写无响应）
        BluetoothGattCharacteristic cp = new BluetoothGattCharacteristic(
                UUID_CONTROL_POINT,
                BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE,
                BluetoothGattDescriptor.PERMISSION_WRITE);
        cp.setValue(new byte[] { 0x00 });
        svc.addCharacteristic(cp);

        // 0x2A4E Protocol Mode（默认 Report 模式）
        protocolModeCh = new BluetoothGattCharacteristic(
                UUID_PROTOCOL_MODE,
                BluetoothGattCharacteristic.PROPERTY_READ
                        | BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE,
                BluetoothGattDescriptor.PERMISSION_READ | BluetoothGattDescriptor.PERMISSION_WRITE);
        protocolModeCh.setValue(new byte[] { PROTOCOL_REPORT });
        svc.addCharacteristic(protocolModeCh);

        // 0x2A4D Report —— 键盘输入（report id 1）
        keyboardReportCh = new BluetoothGattCharacteristic(
                UUID_REPORT,
                BluetoothGattCharacteristic.PROPERTY_READ
                        | BluetoothGattCharacteristic.PROPERTY_WRITE
                        | BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE
                        | BluetoothGattCharacteristic.PROPERTY_NOTIFY,
                BluetoothGattDescriptor.PERMISSION_READ | BluetoothGattDescriptor.PERMISSION_WRITE);
        keyboardReportCh.setValue(new byte[8]);
        addReportReference(keyboardReportCh,
                keyboardOnly ? 0 : HidDescriptors.REPORT_ID_KEYBOARD, REPORT_TYPE_INPUT);
        svc.addCharacteristic(keyboardReportCh);

        // 0x2A4D Report —— 键盘 LED 输出（report id 1, type output）
        BluetoothGattCharacteristic ledOut = new BluetoothGattCharacteristic(
                UUID_REPORT,
                BluetoothGattCharacteristic.PROPERTY_READ
                        | BluetoothGattCharacteristic.PROPERTY_WRITE
                        | BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE,
                BluetoothGattDescriptor.PERMISSION_READ | BluetoothGattDescriptor.PERMISSION_WRITE);
        ledOut.setValue(new byte[] { 0x00 });
        addReportReference(ledOut, keyboardOnly ? 0 : HidDescriptors.REPORT_ID_KEYBOARD, REPORT_TYPE_OUTPUT);
        svc.addCharacteristic(ledOut);

        // 0x2A4D Report —— 鼠标输入（report id 2），兼容模式里不提供
        if (!keyboardOnly) {
            mouseReportCh = new BluetoothGattCharacteristic(
                    UUID_REPORT,
                    BluetoothGattCharacteristic.PROPERTY_READ
                            | BluetoothGattCharacteristic.PROPERTY_WRITE
                            | BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE
                            | BluetoothGattCharacteristic.PROPERTY_NOTIFY,
                    BluetoothGattDescriptor.PERMISSION_READ | BluetoothGattDescriptor.PERMISSION_WRITE);
            mouseReportCh.setValue(new byte[4]);
            addReportReference(mouseReportCh, HidDescriptors.REPORT_ID_MOUSE, REPORT_TYPE_INPUT);
            svc.addCharacteristic(mouseReportCh);
        }

        // Boot 模式报告（Windows 一般走 Report 模式，但缺了有的主机不认）
        bootKeyboardInCh = new BluetoothGattCharacteristic(
                UUID_BOOT_KB_IN,
                BluetoothGattCharacteristic.PROPERTY_READ
                        | BluetoothGattCharacteristic.PROPERTY_NOTIFY,
                BluetoothGattDescriptor.PERMISSION_READ);
        bootKeyboardInCh.setValue(new byte[8]);
        svc.addCharacteristic(bootKeyboardInCh);

        BluetoothGattCharacteristic bootKbOut = new BluetoothGattCharacteristic(
                UUID_BOOT_KB_OUT,
                BluetoothGattCharacteristic.PROPERTY_READ
                        | BluetoothGattCharacteristic.PROPERTY_WRITE
                        | BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE,
                BluetoothGattDescriptor.PERMISSION_READ | BluetoothGattDescriptor.PERMISSION_WRITE);
        bootKbOut.setValue(new byte[] { 0x00 });
        svc.addCharacteristic(bootKbOut);

        if (manualCccd) {
            addCccd(keyboardReportCh);
            if (mouseReportCh != null) {
                addCccd(mouseReportCh);
            }
            addCccd(bootKeyboardInCh);
            diag("已手动为 3 个可通知特征补 CCCD(0x2902)");
        } else {
            diag("未手动加 CCCD，依赖系统自动补");
        }
        diag("Report 特征描述符数：键盘=" + keyboardReportCh.getDescriptors().size()
                + " 鼠标=" + (mouseReportCh == null ? 0 : mouseReportCh.getDescriptors().size()));

        return svc;
    }

    private void addReportReference(BluetoothGattCharacteristic ch, int reportId, int type) {
        BluetoothGattDescriptor d = new BluetoothGattDescriptor(
                UUID_REPORT_REF,
                BluetoothGattDescriptor.PERMISSION_READ);
        d.setValue(new byte[] { (byte) (reportId & 0xFF), (byte) (type & 0xFF) });
        ch.addDescriptor(d);
    }

    private void startAdvertising() {
        ensureAdapter();
        if (adapter == null) {
            return;
        }
        if (advertiser == null) {
            advertiser = adapter.getBluetoothLeAdvertiser();
        }
        if (advertiser == null) {
            setState(STATE_ERROR, "本机不支持 BLE 广播", "请改用经典 HID 模式");
            return;
        }
        try {
            advertiser.stopAdvertising(advertiseCallback);
        } catch (Throwable ignored) {
        }

        // 名字短就直接塞进广播包（有些主机的扫描器只读 ADV_IND 不看 scan response）
        String name = getAdapterName();
        boolean nameInAdv = name != null && name.length() > 0 && name.length() <= 10;

        AdvertiseData adv = new AdvertiseData.Builder()
                .addServiceUuid(new ParcelUuid(UUID_HID_SERVICE))
                .setIncludeDeviceName(nameInAdv)
                .setIncludeTxPowerLevel(false)
                .build();
        AdvertiseData scanResponse = nameInAdv ? null : new AdvertiseData.Builder()
                .setIncludeDeviceName(true)
                .build();
        AdvertiseSettings settings = new AdvertiseSettings.Builder()
                .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
                .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_MEDIUM)
                .setConnectable(true)
                .setTimeout(0)
                .build();
        try {
            advertiser.startAdvertising(settings, adv, scanResponse, advertiseCallback);
            diag("开始广播 0x1812：" + name + (nameInAdv ? "（名字在广播包）" : "（名字在 scan response）"));
        } catch (Throwable t) {
            diag("startAdvertising 异常: " + t);
            setState(STATE_ERROR, "广播失败", String.valueOf(t));
        }
    }

    private final AdvertiseCallback advertiseCallback = new AdvertiseCallback() {
        @Override
        public void onStartSuccess(AdvertiseSettings settingsInEffect) {
            diag("广播已启动");
            if (state != STATE_CONNECTED) {
                setState(STATE_ADVERTISING, "BLE 键盘广播中",
                        "Windows 里添加设备，找「" + getAdapterName() + "」");
            }
        }

        @Override
        public void onStartFailure(int errorCode) {
            String reason;
            switch (errorCode) {
                case ADVERTISE_FAILED_DATA_TOO_LARGE:
                    reason = "广播数据超长";
                    break;
                case ADVERTISE_FAILED_TOO_MANY_ADVERTISERS:
                    reason = "广播实例太多";
                    break;
                case ADVERTISE_FAILED_ALREADY_STARTED:
                    reason = "已经在广播";
                    break;
                case ADVERTISE_FAILED_INTERNAL_ERROR:
                    reason = "内部错误";
                    break;
                case ADVERTISE_FAILED_FEATURE_UNSUPPORTED:
                    reason = "本机不支持 BLE 广播";
                    break;
                default:
                    reason = "错误码 " + errorCode;
                    break;
            }
            diag("广播失败: " + reason);
            setState(STATE_ERROR, "BLE 广播失败", reason + "（可改用经典 HID 模式）");
        }
    };

    public void stop() {
        if (advertiser != null) {
            try {
                advertiser.stopAdvertising(advertiseCallback);
            } catch (Throwable ignored) {
            }
        }
        if (server != null) {
            try {
                BluetoothGattService svc = server.getService(UUID_HID_SERVICE);
                if (svc != null) {
                    server.removeService(svc);
                }
                server.close();
            } catch (Throwable t) {
                diag("关闭 GATT 服务异常: " + t);
            }
        }
        server = null;
        host = null;
        keyboardNotifyEnabled = false;
        mouseNotifyEnabled = false;
        bootNotifyEnabled = false;
        setState(STATE_OFF, "BLE 已停止", "");
    }

    // ---------------- 发送 ----------------

    public synchronized void send(int mods, int[] keys, int count) {
        lastMods = mods & 0xFF;
        int n = Math.max(0, Math.min(count, 6));
        lastCount = n;
        for (int i = 0; i < 6; i++) {
            lastKeys[i] = i < n ? (keys[i] & 0xFF) : 0;
        }
        byte[] payload = buildKeyboardReport();
        if (protocolMode == PROTOCOL_BOOT && bootKeyboardInCh != null) {
            notifyValue(bootKeyboardInCh, payload, bootNotifyEnabled);
        } else if (keyboardReportCh != null) {
            notifyValue(keyboardReportCh, payload, keyboardNotifyEnabled);
        }
    }

    public synchronized void sendMouse(int buttons, int dx, int dy, int wheel) {
        if (keyboardOnly || mouseReportCh == null) {
            return;
        }
        mouseButtons = buttons & 0x07;
        byte[] payload = new byte[4];
        payload[0] = (byte) mouseButtons;
        payload[1] = (byte) clampAxis(dx);
        payload[2] = (byte) clampAxis(dy);
        payload[3] = (byte) clampAxis(wheel);
        notifyValue(mouseReportCh, payload, mouseNotifyEnabled);
    }

    public synchronized void allUp() {
        mouseButtons = 0;
        send(0, new int[0], 0);
        sendMouse(0, 0, 0, 0);
    }

    private byte[] buildKeyboardReport() {
        byte[] r = new byte[8];
        r[0] = (byte) lastMods;
        int n = Math.min(lastCount, 6);
        for (int i = 0; i < n; i++) {
            r[2 + i] = (byte) lastKeys[i];
        }
        return r;
    }

    private static int clampAxis(int v) {
        if (v > 127) {
            return 127;
        }
        return v < -127 ? -127 : v;
    }

    private void notifyValue(BluetoothGattCharacteristic ch, byte[] value, boolean enabled) {
        BluetoothGattServer s = server;
        BluetoothDevice d = host;
        if (s == null || d == null) {
            return;
        }
        if (!enabled) {
            // 主机还没打开通知：静默丢弃，避免刷屏
            return;
        }
        try {
            ch.setValue(value);
            s.notifyCharacteristicChanged(d, ch, false);
        } catch (Throwable t) {
            diag("notify 失败: " + t);
        }
    }

    // ---------------- GATT 回调 ----------------

    private final BluetoothGattServerCallback serverCallback = new BluetoothGattServerCallback() {
        @Override
        public void onConnectionStateChange(BluetoothDevice device, int status, int newState) {
            diag("BLE 连接状态 " + newState + " status=" + status
                    + " 名称=" + label(device)
                    + " 地址=" + address(device)
                    + " 绑定=" + bondState(device));
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                host = device;
                setState(STATE_CONNECTED, "BLE 已连接 · " + label(device),
                        "地址 " + address(device) + " —— 若这不是你的电脑，用 ⋯ →「断开当前 BLE 连接」");
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                host = null;
                keyboardNotifyEnabled = false;
                mouseNotifyEnabled = false;
                bootNotifyEnabled = false;
                setState(STATE_ADVERTISING, "BLE 已断开", "重新在 Windows 里点连接，或等它自己连回来");
                // 断开后重新广播，方便主机重连
                main.postDelayed(new Runnable() {
                    @Override
                    public void run() {
                        if (server != null && host == null) {
                            startAdvertising();
                        }
                    }
                }, 800);
            }
        }

        @Override
        public void onServiceAdded(int status, BluetoothGattService service) {
            diag("onServiceAdded " + service.getUuid() + " status=" + status);
        }

        @Override
        public void onCharacteristicReadRequest(BluetoothDevice device, int requestId,
                                                int offset, BluetoothGattCharacteristic ch) {
            byte[] value = ch.getValue();
            if (value == null) {
                value = new byte[0];
            }
            if (offset > value.length) {
                respondError(device, requestId, offset);
                return;
            }
            byte[] out = new byte[value.length - offset];
            System.arraycopy(value, offset, out, 0, out.length);
            diag("read " + shortUuid(ch.getUuid()) + " len=" + out.length);
            try {
                server.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, out);
            } catch (Throwable t) {
                diag("sendResponse 失败: " + t);
            }
        }

        @Override
        public void onCharacteristicWriteRequest(BluetoothDevice device, int requestId,
                                                 BluetoothGattCharacteristic ch,
                                                 boolean preparedWrite, boolean responseNeeded,
                                                 int offset, byte[] value) {
            byte v = (value != null && value.length > 0) ? value[0] : 0;
            diag("write " + shortUuid(ch.getUuid()) + " = 0x"
                    + String.format(Locale.US, "%02x", v));
            if (UUID_PROTOCOL_MODE.equals(ch.getUuid())) {
                protocolMode = v;
                protocolModeCh.setValue(new byte[] { v });
                diag("主机切到 " + (v == PROTOCOL_BOOT ? "Boot" : "Report") + " 模式");
            }
            if (responseNeeded) {
                try {
                    server.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, value);
                } catch (Throwable t) {
                    diag("sendResponse 失败: " + t);
                }
            }
        }

        @Override
        public void onDescriptorReadRequest(BluetoothDevice device, int requestId,
                                            int offset, BluetoothGattDescriptor descriptor) {
            byte[] value = descriptor.getValue();
            if (value == null) {
                value = new byte[] { 0x00, 0x00 };
            }
            try {
                server.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, value);
            } catch (Throwable t) {
                diag("descriptor read 回复失败: " + t);
            }
        }

        @Override
        public void onDescriptorWriteRequest(BluetoothDevice device, int requestId,
                                             BluetoothGattDescriptor descriptor,
                                             boolean preparedWrite, boolean responseNeeded,
                                             int offset, byte[] value) {
            boolean enable = value != null && value.length > 0
                    && (value[0] & 0x01) != 0;
            BluetoothGattCharacteristic owner = descriptor.getCharacteristic();
            if (owner != null) {
                if (owner == keyboardReportCh) {
                    keyboardNotifyEnabled = enable;
                } else if (owner == mouseReportCh) {
                    mouseNotifyEnabled = enable;
                } else if (owner == bootKeyboardInCh) {
                    bootNotifyEnabled = enable;
                }
                diag("CCCD " + shortUuid(owner.getUuid())
                        + (owner == keyboardReportCh ? "(键盘)"
                        : owner == mouseReportCh ? "(鼠标)"
                        : owner == bootKeyboardInCh ? "(Boot键盘)" : "")
                        + " -> " + (enable ? "通知已开启" : "通知已关闭"));
            }
            if (responseNeeded) {
                try {
                    server.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, value);
                } catch (Throwable t) {
                    diag("descriptor write 回复失败: " + t);
                }
            }
            if (enable && owner == keyboardReportCh) {
                // Windows 打开通知后会期待一个初始状态
                notifyValue(keyboardReportCh, buildKeyboardReport(), true);
            }
        }

        @Override
        public void onNotificationSent(BluetoothDevice device, int status) {
            // 只在出错时记录
            if (status != BluetoothGatt.GATT_SUCCESS) {
                diag("onNotificationSent status=" + status);
            }
        }

        @Override
        public void onMtuChanged(BluetoothDevice device, int mtu) {
            diag("MTU = " + mtu);
        }
    };

    private void respondError(BluetoothDevice device, int requestId, int offset) {
        try {
            server.sendResponse(device, requestId, BluetoothGatt.GATT_INVALID_OFFSET, offset, null);
        } catch (Throwable ignored) {
        }
    }

    private static String shortUuid(UUID u) {
        String s = u.toString();
        if (s.startsWith("0000") && s.endsWith("-0000-1000-8000-00805f9b34fb")) {
            return "0x" + s.substring(4, 8).toUpperCase(Locale.US);
        }
        return s;
    }

    private static String label(BluetoothDevice d) {
        try {
            String n = d.getName();
            return n == null ? d.getAddress() : n;
        } catch (SecurityException e) {
            return "主机";
        }
    }

    private static String address(BluetoothDevice d) {
        try {
            return d.getAddress();
        } catch (SecurityException e) {
            return "?";
        }
    }

    private static String bondState(BluetoothDevice d) {
        try {
            switch (d.getBondState()) {
                case BluetoothDevice.BOND_BONDED:
                    return "已配对";
                case BluetoothDevice.BOND_BONDING:
                    return "配对中";
                default:
                    return "未配对";
            }
        } catch (Throwable t) {
            return "?";
        }
    }

    private void setState(int newState, String title, String detail) {
        state = newState;
        final Listener l = listener;
        if (l == null) {
            return;
        }
        final String t = title;
        final String d = detail;
        main.post(new Runnable() {
            @Override
            public void run() {
                Listener cur = listener;
                if (cur != null) {
                    cur.onBleState(state, t, d);
                }
            }
        });
    }

    private void diag(String msg) {
        Log.i(TAG, msg);
        final Listener l = listener;
        if (l == null) {
            return;
        }
        final String line = msg;
        main.post(new Runnable() {
            @Override
            public void run() {
                Listener cur = listener;
                if (cur != null) {
                    cur.onBleDiag(line);
                }
            }
        });
    }

    private static String titleOf(int s) {
        switch (s) {
            case STATE_ADVERTISING:
                return "BLE 广播中";
            case STATE_CONNECTED:
                return "BLE 已连接";
            case STATE_ERROR:
                return "BLE 异常";
            default:
                return "BLE 未启动";
        }
    }

    private static String detailOf(int s) {
        return s == STATE_OFF ? "点 ⋯ → 切换到 BLE 模式" : "";
    }
}
