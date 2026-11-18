package com.paopao.switchkey;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothHidDevice;
import android.bluetooth.BluetoothHidDeviceAppQosSettings;
import android.bluetooth.BluetoothHidDeviceAppSdpSettings;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.lang.reflect.Method;
import java.util.concurrent.Executor;

/**
 * 把手机注册成一个标准蓝牙 HID 键盘（Bluetooth HID Device Profile, API 28+）。
 *
 * 手机 = HID Device（从设备），Windows 11 = HID Host。
 * 连接成功后，Windows 会把它当成一个真的 USB/蓝牙键盘，
 * 因而输入法（微软拼音）、快捷键、游戏聊天框全部照常工作。
 */
public final class HidKeyboard {

    private static final String TAG = "SwitchKey";

    /** 注册到 Windows 上显示的设备名 */
    public static final String DEVICE_NAME = "SwitchKey";

    public static final int MOD_LCTRL = 0x01;
    public static final int MOD_LSHIFT = 0x02;
    public static final int MOD_LALT = 0x04;
    public static final int MOD_LGUI = 0x08;
    public static final int MOD_RCTRL = 0x10;
    public static final int MOD_RSHIFT = 0x20;
    public static final int MOD_RALT = 0x40;
    public static final int MOD_RGUI = 0x80;

    public static final int STATE_IDLE = 0;
    public static final int STATE_INIT = 1;
    public static final int STATE_READY = 2;
    public static final int STATE_CONNECTED = 3;
    public static final int STATE_UNSUPPORTED = 4;
    public static final int STATE_ERROR = 5;

    public interface Listener {
        void onKeyboardState(int state, String title, String detail);

        /** 诊断日志，一行一条（界面里有「诊断」面板） */
        void onDiag(String line);
    }

    public static final int REPORT_ID_KEYBOARD = HidDescriptors.REPORT_ID_KEYBOARD;
    public static final int REPORT_ID_MOUSE = HidDescriptors.REPORT_ID_MOUSE;

    /** 复合描述符（键盘 + 鼠标）定义在 {@link HidDescriptors} 里，便于单独做解析测试 */
    private static final byte[] REPORT_MAP = HidDescriptors.REPORT_MAP;

    private static HidKeyboard sInstance;

    public static synchronized HidKeyboard get(Context ctx) {
        if (sInstance == null) {
            sInstance = new HidKeyboard(ctx.getApplicationContext());
        }
        return sInstance;
    }

    private final Context appContext;
    private final Handler main = new Handler(Looper.getMainLooper());

    private BluetoothAdapter adapter;
    private BluetoothHidDevice hid;
    private BluetoothDevice host;
    private volatile boolean registered;
    private Listener listener;
    private int state = STATE_IDLE;

    /** 兼容模式：只注册键盘、且不使用 report id（个别主机的 HID 驱动不认复合设备） */
    private volatile boolean keyboardOnly;
    /** 是否收到过主机的 GET/SET_REPORT —— 收到说明主机真的把 HID 驱动挂上了 */
    private volatile boolean hostTouchedHid;

    // 最近一次发送的报告，用于 Windows 主动 GET_REPORT 时回填
    private int lastMods;
    private final int[] lastKeys = new int[6];
    private int lastCount;
    private int mouseButtons;
    private boolean sendFailLogged;

    // 抗掉线：记住上次连上的主机，被别的蓝牙设备挤掉后自动重连
    private static final long KEEPALIVE_MS = 20000L;
    private static final long RECONNECT_MAX_MS = 30000L;
    private BluetoothDevice lastHost;
    private volatile boolean wantConnection;
    private int reconnectAttempt;

    private final Runnable keepAliveTask = new Runnable() {
        @Override
        public void run() {
            BluetoothHidDevice h = hid;
            BluetoothDevice d = host;
            if (h != null && d != null) {
                try {
                    // 发一份内容不变的报告：HID 主机看不出区别，
                    // 但能让蓝牙链路保持活跃，减少被省电策略回收的概率
                    h.sendReport(d, keyboardOnly ? 0 : REPORT_ID_KEYBOARD, buildReport());
                } catch (Throwable ignored) {
                    // 链路正在断，忽略
                }
                main.postDelayed(this, KEEPALIVE_MS);
            }
        }
    };

    private final Runnable reconnectTask = new Runnable() {
        @Override
        public void run() {
            doReconnect();
        }
    };

    private void scheduleReconnect() {
        if (!wantConnection) {
            return;
        }
        long delay = Math.min(RECONNECT_MAX_MS, 1000L << Math.min(reconnectAttempt, 5));
        reconnectAttempt++;
        main.removeCallbacks(reconnectTask);
        main.postDelayed(reconnectTask, delay);
        Log.i(TAG, "计划重连 in " + delay + "ms (attempt " + reconnectAttempt + ")");
    }

    private void doReconnect() {
        if (!wantConnection) {
            return;
        }
        if (host != null) {
            return; // 已经连上了
        }
        ensureAdapter();
        if (adapter == null || !adapter.isEnabled()) {
            setState(STATE_ERROR, "蓝牙已关闭", "打开手机蓝牙后会自动重连");
            scheduleReconnect();
            return;
        }
        if (hid == null) {
            // 蓝牙被关过又打开：重新走一遍注册
            start();
            if (lastHost == null || host == null) {
                scheduleReconnect();
            }
            return;
        }
        if (lastHost == null) {
            return;
        }
        try {
            boolean ok = hid.connect(lastHost);
            Log.i(TAG, "主动重连 connect()=" + ok);
        } catch (Throwable t) {
            Log.w(TAG, "主动重连失败", t);
        }
        if (host == null) {
            scheduleReconnect();
        }
    }

    /** 用户手动要求连接（点「连接上次的电脑」） */
    public void reconnectNow() {
        wantConnection = true;
        reconnectAttempt = 0;
        main.removeCallbacks(reconnectTask);
        main.post(reconnectTask);
    }

    public boolean hasRememberedHost() {
        return lastHost != null;
    }

    /** 手机上已配对的设备（用来手动推一次连接；有些主机不会主动连 HID 设备） */
    public java.util.List<BluetoothDevice> bondedDevices() {
        ensureAdapter();
        java.util.List<BluetoothDevice> out = new java.util.ArrayList<>();
        if (adapter == null) {
            return out;
        }
        try {
            java.util.Set<BluetoothDevice> set = adapter.getBondedDevices();
            if (set != null) {
                out.addAll(set);
            }
        } catch (SecurityException e) {
            diag("读取已配对设备被拒绝: " + e.getMessage());
        }
        return out;
    }

    public String deviceLabel(BluetoothDevice d) {
        return safeName(d) + " · " + safeAddress(d);
    }

    /** 主动连接某个已配对的主机 */
    public void connectTo(BluetoothDevice d) {
        if (d == null) {
            return;
        }
        lastHost = d;
        wantConnection = true;
        reconnectAttempt = 0;
        diag("手动推连接 -> " + deviceLabel(d));
        if (hid == null) {
            diag("HID 还没注册好，先注册再连");
            start();
            main.removeCallbacks(reconnectTask);
            main.postDelayed(reconnectTask, 1500);
            return;
        }
        try {
            boolean ok = hid.connect(d);
            diag("connect() 返回 " + ok);
            if (!ok) {
                scheduleReconnect();
            }
        } catch (Throwable t) {
            diag("connect 异常: " + t);
            scheduleReconnect();
        }
    }

    private HidKeyboard(Context ctx) {
        this.appContext = ctx;
    }

    public void setListener(Listener l) {
        this.listener = l;
        if (l != null) {
            l.onKeyboardState(state, titleOf(state), detailOf(state));
        }
    }

    public int getState() {
        return state;
    }

    public boolean isConnected() {
        return state == STATE_CONNECTED;
    }

    public BluetoothAdapter getAdapter() {
        ensureAdapter();
        return adapter;
    }

    private void ensureAdapter() {
        if (adapter == null) {
            BluetoothManager bm = (BluetoothManager) appContext.getSystemService(Context.BLUETOOTH_SERVICE);
            if (bm != null) {
                adapter = bm.getAdapter();
            }
        }
    }

    private final BluetoothProfile.ServiceListener profileListener = new BluetoothProfile.ServiceListener() {
        @Override
        public void onServiceConnected(int profile, BluetoothProfile proxy) {
            if (profile != BluetoothProfile.HID_DEVICE) {
                return;
            }
            Log.i(TAG, "HID_DEVICE 服务已连接");
            hid = (BluetoothHidDevice) proxy;
            doRegister();
        }

        @Override
        public void onServiceDisconnected(int profile) {
            if (profile != BluetoothProfile.HID_DEVICE) {
                return;
            }
            Log.w(TAG, "HID_DEVICE 服务断开");
            hid = null;
            registered = false;
            host = null;
            setState(STATE_IDLE, "蓝牙 HID 已卸载", "点「启动键盘」重新注册");
        }
    };

    /** 兼容模式切换：换描述符必须重新注册，主机侧也要重新配对 */
    public void setKeyboardOnly(boolean only) {
        if (this.keyboardOnly == only) {
            return;
        }
        diag("切换模式: " + (only ? "仅键盘" : "键盘+鼠标"));
        this.keyboardOnly = only;
        boolean wasWanted = wantConnection;
        stopInternal(false);
        if (wasWanted) {
            wantConnection = true;
        }
        start();
    }

    public boolean isKeyboardOnly() {
        return keyboardOnly;
    }

    public boolean isRegistered() {
        return registered;
    }

    public boolean hostTouchedHid() {
        return hostTouchedHid;
    }

    /** 手机的蓝牙名 —— Windows 上看到的就是这个名字，不是 SwitchKey */
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
                    cur.onDiag(line);
                }
            }
        });
    }

    /** 启动 / 重新注册 HID 设备 */
    public void start() {
        ensureAdapter();
        if (adapter == null) {
            diag("没有蓝牙适配器");
            setState(STATE_UNSUPPORTED, "这台手机没有蓝牙", "无法作为蓝牙键盘使用");
            return;
        }
        if (!adapter.isEnabled()) {
            diag("蓝牙未开启");
            setState(STATE_ERROR, "蓝牙未开启", "请打开蓝牙后再试");
            return;
        }
        if (hid != null) {
            if (!registered) {
                doRegister();
            }
            return;
        }
        diag("getProfileProxy(HID_DEVICE) 请求中…");
        setState(STATE_INIT, "正在初始化蓝牙 HID…", "");
        boolean ok;
        try {
            ok = adapter.getProfileProxy(appContext, profileListener, BluetoothProfile.HID_DEVICE);
        } catch (SecurityException e) {
            ok = false;
            diag("getProfileProxy 被拒绝: " + e.getMessage());
        }
        if (!ok) {
            diag("getProfileProxy 返回 false");
            setState(STATE_UNSUPPORTED, "本机不支持蓝牙 HID 设备模式",
                    "很多国产 ROM 阉割了这个能力。可以换一台手机，"
                            + "或者用「手机当键盘」的 Wi-Fi 方案（见 README）");
        }
    }

    private void doRegister() {
        if (hid == null || registered) {
            return;
        }
        byte[] map = keyboardOnly
                ? HidDescriptors.REPORT_MAP_KEYBOARD_ONLY
                : HidDescriptors.REPORT_MAP;
        diag("registerApp: " + (keyboardOnly ? "仅键盘描述符" : "键盘+鼠标描述符")
                + " " + map.length + " bytes");
        BluetoothHidDeviceAppSdpSettings sdp = new BluetoothHidDeviceAppSdpSettings(
                DEVICE_NAME,
                keyboardOnly ? "SwitchKey keyboard" : "SwitchKey keyboard + touchpad",
                "PaoPao",
                BluetoothHidDevice.SUBCLASS1_KEYBOARD,
                map);
        boolean ok;
        try {
            ok = registerAppCompat(sdp);
        } catch (SecurityException e) {
            ok = false;
            diag("registerApp 被拒绝: " + e.getMessage());
        } catch (Throwable t) {
            ok = false;
            diag("registerApp 异常: " + t);
        }
        if (!ok) {
            diag("registerApp 返回 false —— ROM 可能不支持");
            setState(STATE_UNSUPPORTED, "注册蓝牙键盘失败",
                    "系统拒绝注册 HID 设备，多半是 ROM 不支持");
            return;
        }
        diag("registerApp 已提交，等待系统回调…");
        setState(STATE_INIT, "正在注册…", "等待系统确认");
    }

    private final Executor mainExecutor = new Executor() {
        @Override
        public void execute(Runnable command) {
            main.post(command);
        }
    };

    /**
     * registerApp 在 Android 13 (API 33) 改成了带 Executor 的重载，
     * 旧重载在新 SDK 里已经拿不到，所以这里反射选一个可用的。
     */
    private boolean registerAppCompat(BluetoothHidDeviceAppSdpSettings sdp) throws Exception {
        try {
            Method m = BluetoothHidDevice.class.getMethod("registerApp",
                    BluetoothHidDeviceAppSdpSettings.class,
                    BluetoothHidDeviceAppQosSettings.class,
                    BluetoothHidDeviceAppQosSettings.class,
                    Executor.class,
                    BluetoothHidDevice.Callback.class);
            Object r = m.invoke(hid, sdp, null, null, mainExecutor, callback);
            return r instanceof Boolean && (Boolean) r;
        } catch (NoSuchMethodException notNew) {
            Method m = BluetoothHidDevice.class.getMethod("registerApp",
                    BluetoothHidDeviceAppSdpSettings.class,
                    BluetoothHidDeviceAppQosSettings.class,
                    BluetoothHidDeviceAppQosSettings.class,
                    BluetoothHidDevice.Callback.class);
            Object r = m.invoke(hid, sdp, null, null, callback);
            return r instanceof Boolean && (Boolean) r;
        }
    }

    private final BluetoothHidDevice.Callback callback = new BluetoothHidDevice.Callback() {
        @Override
        public void onAppStatusChanged(BluetoothDevice pluggedDevice, boolean isRegistered) {
            diag("onAppStatusChanged registered=" + isRegistered);
            registered = isRegistered;
            if (isRegistered) {
                wantConnection = true;
                sendFailLogged = false;
                // 注册成功就把前台服务拉起来，避免去 Windows 配对的路上被系统回收
                HidService.ensureRunning(appContext);
                String btName = getAdapterName();
                if (lastHost != null) {
                    setState(STATE_READY, "键盘已就绪", "正在重连上次的电脑，没反应就点「重连」");
                    scheduleReconnect();
                } else {
                    setState(STATE_READY, "键盘已就绪",
                            "点右上角 ⋯ →「让手机可被发现」，Windows 里找「" + btName + "」");
                }
            } else {
                hid = null;
                host = null;
                stopKeepAlive();
                setState(STATE_IDLE, "键盘已注销", "点「启动键盘」重新注册");
            }
        }

        @Override
        public void onConnectionStateChanged(BluetoothDevice device, int newState) {
            diag("连接状态 -> " + newState + " (" + safeName(device) + " "
                    + safeAddress(device) + ")");
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                host = device;
                lastHost = device;
                wantConnection = true;
                reconnectAttempt = 0;
                hostTouchedHid = false;
                main.removeCallbacks(reconnectTask);
                main.removeCallbacks(keepAliveTask);
                main.postDelayed(keepAliveTask, KEEPALIVE_MS);
                allUp();
                String name = safeName(device);
                setState(STATE_CONNECTED, "已连接 · " + name, "直接打字吧");
                HidService.ensureRunning(appContext);
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                boolean unexpected = wantConnection && lastHost != null && host != null;
                allUp();
                host = null;
                hostTouchedHid = false;
                sendFailLogged = false;
                stopKeepAlive();
                if (unexpected) {
                    setState(STATE_READY, "掉线了，正在自动重连…",
                            "多半是被别的蓝牙设备抢了链路，不用手动操作");
                    scheduleReconnect();
                } else {
                    setState(STATE_READY, "已断开", "在 Windows 重新点一下设备即可再连");
                }
            }
        }

        private void stopKeepAlive() {
            main.removeCallbacks(keepAliveTask);
        }

        @Override
        public void onGetReport(BluetoothDevice device, byte type, byte id, int bufferSize) {
            // Windows 连上后通常先 GET_REPORT 摸一下当前状态；按 report id 回对应的负载
            if (!hostTouchedHid) {
                hostTouchedHid = true;
                diag("主机在问 HID 报告 —— 说明 Windows 已经把 HID 驱动挂上了");
            }
            byte[] data = (id == REPORT_ID_MOUSE)
                    ? buildMouseReport(mouseButtons, 0, 0, 0)
                    : buildReport();
            try {
                hid.replyReport(device, BluetoothHidDevice.REPORT_TYPE_INPUT, id, data);
            } catch (Throwable t) {
                diag("replyReport 失败: " + t);
            }
        }

        @Override
        public void onSetReport(BluetoothDevice device, byte type, byte id, byte[] data) {
            // LED 状态（大写锁定等），我们不复刻指示灯，回个成功就行
            if (!hostTouchedHid) {
                hostTouchedHid = true;
                diag("主机在写 HID 报告 —— HID 链路是通的");
            }
            try {
                if (type == BluetoothHidDevice.REPORT_TYPE_OUTPUT) {
                    hid.replyReport(device, BluetoothHidDevice.REPORT_TYPE_OUTPUT, id, data);
                } else {
                    hid.reportError(device, BluetoothHidDevice.ERROR_RSP_SUCCESS);
                }
            } catch (Throwable t) {
                diag("onSetReport 回复失败: " + t);
            }
        }

        @Override
        public void onSetProtocol(BluetoothDevice device, byte protocol) {
            diag("主机切换 HID 协议: " + protocol);
        }

        @Override
        public void onInterruptData(BluetoothDevice device, byte reportId, byte[] data) {
            diag("主机发来 interrupt data id=" + reportId
                    + " len=" + (data == null ? 0 : data.length));
        }

        @Override
        public void onVirtualCableUnplug(BluetoothDevice device) {
            diag("主机断开了虚拟线缆");
            host = null;
            setState(STATE_READY, "连接已断开", "在 Windows 上重新连接");
        }
    };

    private byte[] buildReport() {
        byte[] r = new byte[8];
        r[0] = (byte) lastMods;
        int n = Math.min(lastCount, 6);
        for (int i = 0; i < n; i++) {
            r[2 + i] = (byte) lastKeys[i];
        }
        return r;
    }

    /**
     * 发送一次完整按键状态。UI 层每次按键变化都调用一次。
     *
     * @param mods   修饰键位图（MOD_*）
     * @param keys   当前按下的 HID usage 列表
     * @param count  有效个数（<= 6）
     */
    public synchronized void send(int mods, int[] keys, int count) {
        lastMods = mods & 0xFF;
        int n = Math.max(0, Math.min(count, 6));
        lastCount = n;
        for (int i = 0; i < 6; i++) {
            lastKeys[i] = i < n ? (keys[i] & 0xFF) : 0;
        }
        BluetoothHidDevice h = hid;
        BluetoothDevice d = host;
        if (h == null || d == null) {
            return;
        }
        try {
            boolean ok = h.sendReport(d, keyboardOnly ? 0 : REPORT_ID_KEYBOARD, buildReport());
            if (!ok && !sendFailLogged) {
                sendFailLogged = true;
                diag("sendReport 返回 false（主机连接还没建立好）");
            }
        } catch (Throwable t) {
            if (!sendFailLogged) {
                sendFailLogged = true;
                diag("sendReport 异常: " + t);
            }
        }
    }

    /**
     * 发送一次鼠标相对位移/按键/滚轮。
     * 位移是相对值：dx>0 向右，dy>0 向下（HID 约定）。
     */
    public synchronized void sendMouse(int buttons, int dx, int dy, int wheel) {
        if (keyboardOnly) {
            return; // 兼容模式里没有鼠标报告
        }
        mouseButtons = buttons & 0x07;
        BluetoothHidDevice h = hid;
        BluetoothDevice d = host;
        if (h == null || d == null) {
            return;
        }
        try {
            h.sendReport(d, REPORT_ID_MOUSE, buildMouseReport(mouseButtons, dx, dy, wheel));
        } catch (Throwable t) {
            if (!sendFailLogged) {
                sendFailLogged = true;
                diag("sendReport(mouse) 异常: " + t);
            }
        }
    }

    private static byte[] buildMouseReport(int buttons, int dx, int dy, int wheel) {
        byte[] r = new byte[4];
        r[0] = (byte) (buttons & 0x07);
        r[1] = (byte) clampAxis(dx);
        r[2] = (byte) clampAxis(dy);
        r[3] = (byte) clampAxis(wheel);
        return r;
    }

    private static int clampAxis(int v) {
        if (v > 127) {
            return 127;
        }
        return v < -127 ? -127 : v;
    }

    /** 松开全部按键（断开、界面退出时避免按键卡住） */
    public synchronized void allUp() {
        mouseButtons = 0;
        send(0, new int[0], 0);
        sendMouse(0, 0, 0, 0);
    }

    public void stop() {
        stopInternal(true);
        setState(STATE_IDLE, "已停止", "点「启动键盘」重新注册");
    }

    private void stopInternal(boolean clearWant) {
        allUp();
        if (hid != null) {
            try {
                hid.unregisterApp();
                diag("已注销 HID 应用");
            } catch (Throwable t) {
                diag("unregisterApp 失败: " + t);
            }
        }
        if (adapter != null && hid != null) {
            try {
                adapter.closeProfileProxy(BluetoothProfile.HID_DEVICE, hid);
            } catch (Throwable t) {
                diag("closeProfileProxy 失败: " + t);
            }
        }
        hid = null;
        registered = false;
        host = null;
        hostTouchedHid = false;
        if (clearWant) {
            wantConnection = false;
        }
        main.removeCallbacks(reconnectTask);
        main.removeCallbacks(keepAliveTask);
    }

    private void setState(int newState, String title, String detail) {
        state = newState;
        final Listener l = listener;
        if (l == null) {
            return;
        }
        main.post(new Runnable() {
            @Override
            public void run() {
                Listener cur = listener;
                if (cur != null) {
                    cur.onKeyboardState(state, title, detail);
                }
            }
        });
    }

    private static String titleOf(int s) {
        switch (s) {
            case STATE_CONNECTED: return "已连接";
            case STATE_READY: return "键盘已就绪";
            default: return "未连接";
        }
    }

    private static String detailOf(int s) {
        return s == STATE_IDLE ? "点「启动键盘」开始" : "";
    }

    private static String safeName(BluetoothDevice d) {
        try {
            String n = d.getName();
            return n == null ? safeAddress(d) : n;
        } catch (SecurityException e) {
            return "电脑";
        }
    }

    private static String safeAddress(BluetoothDevice d) {
        if (d == null) {
            return "-";
        }
        try {
            return d.getAddress();
        } catch (SecurityException e) {
            return "?";
        }
    }
}
