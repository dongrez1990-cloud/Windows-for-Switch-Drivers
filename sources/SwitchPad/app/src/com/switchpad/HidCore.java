package com.switchpad;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothHidDevice;
import android.bluetooth.BluetoothHidDeviceAppQosSettings;
import android.bluetooth.BluetoothHidDeviceAppSdpSettings;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.util.Log;

import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * 蓝牙 HID 从设备核心。
 *
 * 数据流：手机（HID Device）--L2CAP HID Control/Interrupt--> 电脑（HID Host，即跑 Win11 的 Switch）。
 * Android 从 9.0(API 28) 起提供 {@link BluetoothHidDevice} 打从设备角色，系统内置的
 * 报告描述符与本项目 {@link HidDescriptor} 一致（键盘 1 / 鼠标 2 / 消费类 3）。
 *
 * 线程模型：
 * <ul>
 *   <li>UI 线程只负责改本地状态并提交发送请求；</li>
 *   <li>所有 HID 发送都在 worker 线程上串行执行；</li>
 *   <li>鼠标位移在 8ms 窗口内合并，避免把 120Hz 的触摸事件原样灌进 L2CAP 中断通道。</li>
 * </ul>
 */
public final class HidCore {

    private static final String TAG = "SwitchPad";

    /** 鼠标合并窗口（毫秒） */
    private static final int MOUSE_FLUSH_MS = 8;
    /** 按键按下->抬起的保底间隔，保证主机能看到两次报告 */
    private static final int TAP_GAP_MS = 16;
    /** 注册失败后的最多尝试次数与重试间隔 */
    private static final int MAX_REGISTER_ATTEMPTS = 3;
    private static final int RETRY_DELAY_MS = 1000;
    /** connect() 去重与失败冷却窗口 */
    private static final long CONNECT_COOLDOWN_MS = 6000L;
    /** 配对完成后等主机先动手，再考虑自己发起 */
    private static final int BOND_SETTLE_MS = 2000;

    public interface Listener {
        /** HID 应用注册状态变化 */
        void onHidState(boolean registered, String message);

        /** 与电脑的连接状态变化 */
        void onConnection(boolean connected, String hostName);

        /** 键盘 LED 状态位（1=NumLock 2=CapsLock 4=ScrollLock） */
        void onLeds(int bits);
    }

    /**
     * 反向配对的旁路监听：有些主机（比如从 Linux/BlueZ 移植的 WoA 蓝牙驱动）
     * 在自己发起配对时行为不稳，这时可以让手机主动扫描电脑并发起绑定。
     */
    public interface ScanListener {
        void onDeviceFound(BluetoothDevice device, short rssi);

        void onDiscoveryState(boolean discovering);

        void onBondChanged(BluetoothDevice device, int bondState);
    }

    private static volatile HidCore sInstance;

    public static HidCore get(Context context) {
        HidCore local = sInstance;
        if (local == null) {
            synchronized (HidCore.class) {
                local = sInstance;
                if (local == null) {
                    local = new HidCore(context.getApplicationContext());
                    sInstance = local;
                }
            }
        }
        return local;
    }

    public static HidCore peek() {
        return sInstance;
    }

    /* ------------------------------------------------------------------ */

    private final Context app;
    private final BluetoothAdapter adapter;
    private final HandlerThread workerThread;
    private final Handler worker;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final CopyOnWriteArrayList<Listener> listeners = new CopyOnWriteArrayList<Listener>();
    private final CopyOnWriteArrayList<ScanListener> scanListeners = new CopyOnWriteArrayList<ScanListener>();
    private boolean receiversRegistered;
    /** getProfileProxy 在飞：避免服务与界面各发一次导致重复代理/重复注册 */
    private boolean proxyPending;

    private final KeyboardReport keyboard = new KeyboardReport();
    private final PointerModel pointer = new PointerModel();
    private final ScrollModel scroll = new ScrollModel();

    private BluetoothHidDevice hid;
    private volatile boolean registered;
    private volatile boolean connected;
    private volatile BluetoothDevice host;
    private volatile String hostName = "";
    private int leds;
    private byte currentProtocol = (byte) BluetoothHidDevice.PROTOCOL_REPORT_MODE;

    /* 鼠标合并状态（受 mouseLock 保护） */
    private final Object mouseLock = new Object();
    private int accX;
    private int accY;
    private int accWheel;
    private boolean flushScheduled;
    private volatile int mouseButtons;
    /** 上一次真正发出去的按键位；纯按键变化也必须发一帧，所以要和它比 */
    private volatile int lastSentButtons;

    private String lastStateMessage = "未初始化";

    /* 连接过程中的事件日志（排查"连上又断开"用） */
    private final java.util.ArrayDeque<String> eventLog = new java.util.ArrayDeque<String>();
    private static final int EVENT_LOG_MAX = 24;
    private volatile boolean connecting;
    private long connectStartedAt;
    private volatile long lastConnectFailAt;
    private volatile BluetoothDevice lastTarget;

    /** 记录一条带时间戳的事件，显示在诊断块里 */
    private void event(String what) {
        String line = new java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.US)
                .format(new java.util.Date()) + "  " + what;
        Log.i(TAG, "event: " + what);
        synchronized (eventLog) {
            eventLog.addFirst(line);
            while (eventLog.size() > EVENT_LOG_MAX) eventLog.removeLast();
        }
    }

    public String eventLogText() {
        StringBuilder sb = new StringBuilder();
        synchronized (eventLog) {
            for (String s : eventLog) sb.append(s).append('\n');
        }
        return sb.toString().trim();
    }

    /* 注册诊断 */
    private volatile int registerAttempts;
    private volatile String lastRegisterError = "";
    private volatile boolean statusCallbackSeen;
    /** 期望处于"已注册"状态；stop() 置 false 后就不再自动重试 */
    private volatile boolean wantRegistered;

    private HidCore(Context app) {
        this.app = app;
        BluetoothManager bm = (BluetoothManager) app.getSystemService(Context.BLUETOOTH_SERVICE);
        this.adapter = bm != null ? bm.getAdapter() : null;

        workerThread = new HandlerThread("switchpad-hid");
        workerThread.start();
        worker = new Handler(workerThread.getLooper());

        applySettings();
    }

    /* ------------------------------ 生命周期 ------------------------------ */

    public boolean hasBluetooth() {
        return adapter != null;
    }

    public BluetoothAdapter adapter() {
        return adapter;
    }

    public boolean isRegistered() {
        return registered;
    }

    public boolean isConnected() {
        return connected;
    }

    public String hostName() {
        return hostName;
    }

    public int leds() {
        return leds;
    }

    public KeyboardReport keyboard() {
        return keyboard;
    }

    public String stateMessage() {
        return lastStateMessage;
    }

    /** 从 Prefs 同步灵敏度/加速/滚轮参数 */
    public void applySettings() {
        pointer.setSensitivity(Prefs.sensitivity(app));
        pointer.setAcceleration(Prefs.acceleration(app));
        scroll.setNotchPx(Prefs.scrollNotchPx(app));
        scroll.setGain(Prefs.scrollGain(app));
        scroll.setNatural(Prefs.naturalScroll(app));
    }

    /** 打开蓝牙 Profile 代理并注册 HID 应用（幂等）。 */
    public void start() {
        wantRegistered = true;
        event("start()");
        if (adapter == null) {
            notifyState(false, "本机没有可用的蓝牙适配器");
            return;
        }
        if (!adapter.isEnabled()) {
            notifyState(false, "蓝牙未开启");
            return;
        }
        if (hid != null) {
            if (!registered) registerAppWithRetry();
            else notifyState(true, "HID 已就绪，可以从电脑配对了");
            return;
        }
        if (proxyPending) {
            event("start() 忽略：代理请求已在飞");
            return;
        }
        proxyPending = true;
        try {
            boolean ok = adapter.getProfileProxy(app, serviceListener, BluetoothProfile.HID_DEVICE);
            if (!ok) {
                proxyPending = false;
                notifyState(false, "无法连接 HID Device 服务（该机型可能禁用了 HID 从设备角色）");
            } else {
                notifyState(false, "正在初始化 HID 服务…");
            }
        } catch (SecurityException e) {
            proxyPending = false;
            notifyState(false, "缺少蓝牙权限：" + e.getMessage());
        } catch (Exception e) {
            proxyPending = false;
            notifyState(false, "初始化失败：" + e);
        }
    }

    public boolean hasProxy() {
        return hid != null;
    }

    /**
     * 注册失败最常见的原因（按概率）：
     * 1) 蓝牙栈里还留着上一次注册的 stale 记录（上一个 HID App 被强杀/崩溃、或刚关过蓝牙）；
     * 2) 另一个 App 正占着 HID Device 角色（同时只允许一个）；
     * 3) 蓝牙刚打开、HID 接口还没 ready。
     * 三种都是"等一下/重来一次"能好，所以这里做最多 3 次自动重试。
     */
    private void registerAppWithRetry() {
        registerAttempts = 0;
        lastRegisterError = "";
        attemptRegister();
    }

    private void attemptRegister() {
        if (hid == null) return;
        registerAttempts++;
        statusCallbackSeen = false;
        lastRegisterError = "";
        registerApp();

        if (registered || statusCallbackSeen) return; // 成功或已在等回调
        if (lastRegisterError.isEmpty()) return;      // 已提交，等系统回调

        if (registerAttempts < MAX_REGISTER_ATTEMPTS) {
            notifyState(false, "注册失败（第 " + registerAttempts + " 次），"
                    + (RETRY_DELAY_MS / 1000) + " 秒后重试…");
            worker.postDelayed(registerRetry, RETRY_DELAY_MS);
        } else {
            notifyState(false, "注册 HID 应用失败：" + lastRegisterError
                    + "。可试「重置 HID」，或关一次手机蓝牙后用「重试注册」");
        }
    }

    private final Runnable registerRetry = new Runnable() {
        @Override
        public void run() {
            attemptRegister();
        }
    };

    /**
     * 把 HID 代理整个拆掉重建，用来清掉"上一次注册残留"这种卡死状态。
     * 先 unregister，再关代理，最后重新走一次 start()。
     */
    public void resetHid() {
        wantRegistered = true;
        event("resetHid()");
        worker.removeCallbacks(registerRetry);
        try {
            if (hid != null && registered) hid.unregisterApp();
        } catch (Exception e) {
            Log.w(TAG, "reset unregisterApp", e);
        }
        try {
            if (hid != null && adapter != null) {
                adapter.closeProfileProxy(BluetoothProfile.HID_DEVICE, hid);
            }
        } catch (Exception e) {
            Log.w(TAG, "reset closeProfileProxy", e);
        }
        hid = null;
        proxyPending = false;
        registered = false;
        connected = false;
        host = null;
        hostName = "";
        keyboard.releaseAll();
        mouseButtons = 0;
        notifyConnection(false, "");
        notifyState(false, "已重置 HID 服务，正在重新注册…");
        worker.postDelayed(new Runnable() {
            @Override
            public void run() {
                start();
            }
        }, 400);
    }

    /**
     * 当前主机的链路类型。HID Device 角色只会走经典 BR/EDR，
     * 所以只要能连上，这里必然显示"经典"，不会出现 LE —— 这正是对 BT 3.0 主机友好之处。
     */
    public String linkType() {
        BluetoothDevice d = host;
        if (d == null) return "—";
        try {
            switch (d.getType()) {
                case BluetoothDevice.DEVICE_TYPE_CLASSIC:
                    return "经典 BR/EDR";
                case BluetoothDevice.DEVICE_TYPE_DUAL:
                    return "双模";
                case BluetoothDevice.DEVICE_TYPE_LE:
                    return "LE";
                default:
                    return "未知";
            }
        } catch (Exception e) {
            return "未知";
        }
    }

    /** 给排查面板用的人类可读状态。 */
    public String diagnosis() {
        StringBuilder sb = new StringBuilder();
        sb.append("蓝牙适配器：").append(adapter == null ? "无"
                : (adapter.isEnabled() ? "已开启" : "已关闭")).append('\n');
        sb.append("HID 代理：").append(hid == null ? "未建立" : "已建立").append('\n');
        sb.append("应用注册：").append(registered ? "成功" : "未成功").append('\n');
        sb.append("注册尝试：").append(registerAttempts).append(" 次\n");
        sb.append("描述符：").append(Prefs.compactDescriptor(app) ? "精简(仅键盘)" : "完整(键盘+鼠标)").append('\n');
        if (!lastRegisterError.isEmpty()) {
            sb.append("最近错误：").append(lastRegisterError).append('\n');
        }
        if (hid != null && !registered) {
            sb.append("系统回调：").append(statusCallbackSeen
                    ? "收到 onAppStatusChanged" : "未收到 onAppStatusChanged").append('\n');
        }
        try {
            if (adapter != null) {
                sb.append("HID 通道：")
                        .append(profileStateName(adapter.getProfileConnectionState(BluetoothProfile.HID_DEVICE)))
                        .append('\n');
            }
        } catch (Exception ignored) {
            // 个别 ROM 会抛 SecurityException，忽略即可
        }
        if (connected) {
            sb.append("当前主机：").append(hostName).append('\n');
            sb.append("链路类型：").append(linkType()).append('\n');
        }
        if (connecting) sb.append("连接中…\n");
        String log = eventLogText();
        if (!log.isEmpty()) {
            sb.append("—— 事件日志 ——\n").append(log);
        }
        return sb.toString().trim();
    }

    private static String profileStateName(int state) {
        switch (state) {
            case BluetoothProfile.STATE_CONNECTED:
                return "已连接";
            case BluetoothProfile.STATE_CONNECTING:
                return "连接中";
            case BluetoothProfile.STATE_DISCONNECTING:
                return "断开中";
            case BluetoothProfile.STATE_DISCONNECTED:
                return "未连接";
            default:
                return "未知(" + state + ")";
        }
    }

    /** 释放 HID 应用与代理。连接会被断开。 */
    public void stop() {
        wantRegistered = false;
        try {
            if (hid != null && registered) hid.unregisterApp();
        } catch (Exception e) {
            Log.w(TAG, "unregisterApp", e);
        }
        try {
            if (hid != null && adapter != null) adapter.closeProfileProxy(BluetoothProfile.HID_DEVICE, hid);
        } catch (Exception e) {
            Log.w(TAG, "closeProfileProxy", e);
        }
        hid = null;
        registered = false;
        connected = false;
        host = null;
        hostName = "";
    }

    public void shutdown() {
        unregisterReceivers();
        stop();
        synchronized (mouseLock) {
            accX = 0;
            accY = 0;
            accWheel = 0;
        }
        workerThread.quitSafely();
    }

    private final BluetoothProfile.ServiceListener serviceListener = new BluetoothProfile.ServiceListener() {
        @Override
        public void onServiceConnected(int profile, BluetoothProfile proxy) {
            if (profile != BluetoothProfile.HID_DEVICE) return;
            hid = (BluetoothHidDevice) proxy;
            proxyPending = false;
            event("HID 代理已建立");
            registerAppWithRetry();
        }

        @Override
        public void onServiceDisconnected(int profile) {
            if (profile != BluetoothProfile.HID_DEVICE) return;
            hid = null;
            proxyPending = false;
            registered = false;
            connected = false;
            host = null;
            notifyState(false, "HID 服务已断开");
            notifyConnection(false, "");
        }
    };

    private void registerApp() {
        BluetoothHidDevice h = hid;
        if (h == null) {
            lastRegisterError = "HID 代理未建立";
            return;
        }
        boolean compact = Prefs.compactDescriptor(app);
        byte subclass = compact ? HidDescriptor.SUBCLASS_KEYBOARD : HidDescriptor.SUBCLASS_COMBO;
        BluetoothHidDeviceAppSdpSettings sdp = new BluetoothHidDeviceAppSdpSettings(
                "SwitchPad",
                compact ? "SwitchPad keyboard" : "SwitchPad phone keyboard + touchpad",
                "SwitchPad",
                subclass,
                HidDescriptor.reportDescriptor(compact));
        event("注册描述符：" + (compact ? "精简(仅键盘," : "完整(键盘+鼠标+媒体,")
                + HidDescriptor.length(compact) + "B) 子类=0x"
                + Integer.toHexString(subclass & 0xFF));
        try {
            boolean ok = registerAppCompat(h, sdp);
            if (ok) {
                lastRegisterError = "";
            } else if (registered) {
                // 已经注册着再注册一次必然被拒，这不是错误，别在诊断里误导人
                lastRegisterError = "";
            } else {
                lastRegisterError = "系统拒绝注册（角色被别处占用或蓝牙栈残留）";
            }
            event("registerApp 返回 " + ok + "（第 " + registerAttempts + " 次，当前注册状态=" + registered + "）");
        } catch (SecurityException e) {
            lastRegisterError = "缺少蓝牙权限：" + e.getMessage();
            event("registerApp 权限不足：" + e.getMessage());
        } catch (Exception e) {
            lastRegisterError = "反射调用异常：" + e;
            event("registerApp 异常：" + e);
        }
    }

    /**
     * registerApp 在 API 33 有过一次签名变更：旧版用 Handler 回调线程，新版改成 Executor。
     * 两个重载在编译期并不共存（android-34 的 stub 只留下了 Executor 版本），
     * 所以这里用反射按顺序尝试，保证 Android 9 ~ 15 都能注册成功。
     */
    private boolean registerAppCompat(BluetoothHidDevice h, BluetoothHidDeviceAppSdpSettings sdp)
            throws Exception {
        try {
            java.lang.reflect.Method m = BluetoothHidDevice.class.getMethod("registerApp",
                    BluetoothHidDeviceAppSdpSettings.class,
                    BluetoothHidDeviceAppQosSettings.class,
                    BluetoothHidDeviceAppQosSettings.class,
                    BluetoothHidDevice.Callback.class,
                    Handler.class);
            Object r = m.invoke(h, sdp, null, null, callback, worker);
            return Boolean.TRUE.equals(r);
        } catch (NoSuchMethodException ignored) {
            // 落到 Executor 版本
        }

        final java.util.concurrent.Executor exec = new java.util.concurrent.Executor() {
            @Override
            public void execute(Runnable command) {
                worker.post(command);
            }
        };
        java.lang.reflect.Method m = BluetoothHidDevice.class.getMethod("registerApp",
                BluetoothHidDeviceAppSdpSettings.class,
                BluetoothHidDeviceAppQosSettings.class,
                BluetoothHidDeviceAppQosSettings.class,
                java.util.concurrent.Executor.class,
                BluetoothHidDevice.Callback.class);
        Object r = m.invoke(h, sdp, null, null, exec, callback);
        return Boolean.TRUE.equals(r);
    }

    /* ------------------------------ 连接控制 ------------------------------ */

    /**
     * 主动连接到已配对的目标主机。
     *
     * 关键点：绝不能和主机的"反向发起"打架。配对刚完成时 Windows 往往自己就会来连，
     * 如果这时手机再发一次 connect()，两条 L2CAP 建链请求会互相把对方顶掉，
     * 表现就是"连接一会儿然后断开"。所以这里做了去重 + 冷却。
     */
    public void connect(BluetoothDevice device) {
        BluetoothHidDevice h = hid;
        if (h == null || device == null) return;
        if (connected) return;
        long now = android.os.SystemClock.elapsedRealtime();
        if (connecting && now - connectStartedAt < CONNECT_COOLDOWN_MS) {
            event("忽略重复 connect（上一次尝试仍在进行）");
            return;
        }
        if (now - lastConnectFailAt < CONNECT_COOLDOWN_MS) {
            event("connect 冷却中，跳过");
            return;
        }
        connecting = true;
        connectStartedAt = now;
        lastTarget = device;
        try {
            boolean ok = h.connect(device);
            event("connect(" + safeName(device) + ") = " + ok);
            notifyState(registered, ok ? "正在连接 " + safeName(device) + " …" : "连接请求被拒绝");
        } catch (SecurityException e) {
            connecting = false;
            event("connect 权限不足：" + e.getMessage());
            notifyState(registered, "缺少蓝牙权限：" + e.getMessage());
        } catch (Exception e) {
            connecting = false;
            event("connect 异常：" + e);
            notifyState(registered, "连接失败：" + e);
        }
    }

    /** 供 UI 的「再次连接」按钮调用：清掉冷却后立刻重试。 */
    public void reconnectNow() {
        lastConnectFailAt = 0;
        connecting = false;
        BluetoothDevice target = lastTarget != null ? lastTarget : host;
        if (target == null) {
            List<BluetoothDevice> bonded = bondedDevices();
            if (bonded.isEmpty()) {
                notifyState(registered, "没有已配对设备，请先配对");
                return;
            }
            target = bonded.get(0);
        }
        connect(target);
    }



    public void disconnect() {
        BluetoothHidDevice h = hid;
        BluetoothDevice d = host;
        if (h == null || d == null) return;
        try {
            h.disconnect(d);
        } catch (Exception e) {
            Log.w(TAG, "disconnect", e);
        }
    }

    /** 查询系统当前已连接的 HID 主机（用于恢复状态）。 */
    public void reconcileConnection() {
        BluetoothHidDevice h = hid;
        if (h == null) return;
        try {
            List<BluetoothDevice> devices = h.getConnectedDevices();
            if (devices != null && !devices.isEmpty()) {
                BluetoothDevice d = devices.get(0);
                boolean wasConnected = connected;
                host = d;
                hostName = safeName(d);
                connected = true;
                if (!wasConnected) notifyConnection(true, hostName);
            }
        } catch (Exception e) {
            Log.w(TAG, "reconcileConnection", e);
        }
    }

    /* ---------------------- 反向配对：手机扫描电脑并发起绑定 ---------------------- */

    public void addScanListener(ScanListener l) {
        if (l != null && !scanListeners.contains(l)) scanListeners.add(l);
    }

    public void removeScanListener(ScanListener l) {
        scanListeners.remove(l);
    }

    public void registerReceivers() {
        if (receiversRegistered) return;
        IntentFilter f = new IntentFilter();
        f.addAction(BluetoothAdapter.ACTION_DISCOVERY_STARTED);
        f.addAction(BluetoothAdapter.ACTION_DISCOVERY_FINISHED);
        f.addAction(BluetoothDevice.ACTION_FOUND);
        f.addAction(BluetoothDevice.ACTION_BOND_STATE_CHANGED);
        f.addAction(BluetoothDevice.ACTION_UUID);
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                app.registerReceiver(btReceiver, f, Context.RECEIVER_NOT_EXPORTED);
            } else {
                app.registerReceiver(btReceiver, f);
            }
            receiversRegistered = true;
        } catch (Exception e) {
            Log.w(TAG, "registerReceiver", e);
        }
    }

    public void unregisterReceivers() {
        if (!receiversRegistered) return;
        try {
            app.unregisterReceiver(btReceiver);
        } catch (Exception e) {
            Log.w(TAG, "unregisterReceiver", e);
        }
        receiversRegistered = false;
    }

    /** 开始经典设备发现（手机主动找电脑）。 */
    public boolean startDiscovery() {
        if (adapter == null || !adapter.isEnabled()) {
            notifyState(registered, "蓝牙未开启，无法扫描");
            return false;
        }
        registerReceivers();
        try {
            if (adapter.isDiscovering()) adapter.cancelDiscovery();
            return adapter.startDiscovery();
        } catch (SecurityException e) {
            notifyState(registered, "缺少扫描权限：" + e.getMessage());
            return false;
        } catch (Exception e) {
            notifyState(registered, "扫描失败：" + e);
            return false;
        }
    }

    public void stopDiscovery() {
        try {
            if (adapter != null && adapter.isDiscovering()) adapter.cancelDiscovery();
        } catch (Exception ignored) {
        }
    }

    /**
     * 由手机发起绑定。绑定成功后 {@link #btReceiver} 会自动 connect()，
     * 因为有些主机只会响应"由对端发起"的配对流程。
     */
    public boolean bond(BluetoothDevice device) {
        if (device == null) return false;
        try {
            return device.createBond();
        } catch (SecurityException e) {
            notifyState(registered, "缺少蓝牙权限：" + e.getMessage());
            return false;
        } catch (Exception e) {
            notifyState(registered, "发起配对失败：" + e);
            return false;
        }
    }

    /**
     * 取消配对。removeBond() 是 @SystemApi，公开 SDK 里没有，只能反射；
     * 被隐藏 API 策略拦住时返回 false，由 UI 引导用户去系统蓝牙设置里删。
     */
    public boolean removeBond(BluetoothDevice device) {
        if (device == null) return false;
        try {
            java.lang.reflect.Method m = BluetoothDevice.class.getMethod("removeBond");
            Object r = m.invoke(device);
            event("removeBond(" + safeName(device) + ") = " + r);
            return Boolean.TRUE.equals(r);
        } catch (Throwable t) {
            event("removeBond 不可用：" + t.getClass().getSimpleName());
            return false;
        }
    }

    /** 主动查询对端 SDP（结果走 ACTION_UUID 广播，会记进事件日志）。 */
    public void queryUuids(BluetoothDevice device) {
        if (device == null) return;
        try {
            boolean ok = device.fetchUuidsWithSdp();
            event("fetchUuidsWithSdp(" + safeName(device) + ") = " + ok);
        } catch (Throwable t) {
            event("fetchUuidsWithSdp 失败：" + t.getClass().getSimpleName());
        }
    }

    public static String bondStateName(int state) {
        switch (state) {
            case BluetoothDevice.BOND_BONDED:
                return "已配对";
            case BluetoothDevice.BOND_BONDING:
                return "配对中";
            case BluetoothDevice.BOND_NONE:
            default:
                return "未配对";
        }
    }

    private final BroadcastReceiver btReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent == null ? null : intent.getAction();
            if (action == null) return;

            if (BluetoothDevice.ACTION_FOUND.equals(action)) {
                BluetoothDevice d = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
                short rssi = intent.getShortExtra(BluetoothDevice.EXTRA_RSSI, (short) 0);
                if (d != null) {
                    for (ScanListener l : scanListeners) l.onDeviceFound(d, rssi);
                }
            } else if (BluetoothAdapter.ACTION_DISCOVERY_STARTED.equals(action)) {
                for (ScanListener l : scanListeners) l.onDiscoveryState(true);
            } else if (BluetoothAdapter.ACTION_DISCOVERY_FINISHED.equals(action)) {
                for (ScanListener l : scanListeners) l.onDiscoveryState(false);
            } else if (BluetoothDevice.ACTION_UUID.equals(action)) {
                BluetoothDevice d = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
                android.os.Parcelable[] raw =
                        intent.getParcelableArrayExtra(BluetoothDevice.EXTRA_UUID);
                StringBuilder uuids = new StringBuilder();
                if (raw != null) {
                    for (android.os.Parcelable p : raw) {
                        if (uuids.length() > 60) {
                            uuids.append(" …");
                            break;
                        }
                        uuids.append(shortUuid(String.valueOf(p))).append(' ');
                    }
                }
                event("UUID(" + (d == null ? "?" : safeName(d)) + "): "
                        + (uuids.length() == 0 ? "空（SDP 查询失败）" : uuids.toString().trim()));
            } else if (BluetoothDevice.ACTION_BOND_STATE_CHANGED.equals(action)) {
                BluetoothDevice d = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
                int state = intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.BOND_NONE);
                if (d == null) return;
                for (ScanListener l : scanListeners) l.onBondChanged(d, state);
                if (state == BluetoothDevice.BOND_BONDED) {
                    event("BOND_BONDED " + safeName(d));
                    notifyState(true, "已与 " + safeName(d) + " 完成绑定，等待 HID 连接…");
                    lastTarget = d;
                    final BluetoothDevice target = d;
                    // 给主机 2 秒自己发起，避免两边同时建链互相顶掉
                    worker.postDelayed(new Runnable() {
                        @Override
                        public void run() {
                            if (!connected) connect(target);
                        }
                    }, BOND_SETTLE_MS);
                }
            }
        }
    };

    public List<BluetoothDevice> bondedDevices() {
        if (adapter == null) return java.util.Collections.emptyList();
        try {
            java.util.Set<BluetoothDevice> set = adapter.getBondedDevices();
            return set == null ? java.util.Collections.<BluetoothDevice>emptyList()
                    : new java.util.ArrayList<BluetoothDevice>(set);
        } catch (SecurityException e) {
            return java.util.Collections.emptyList();
        }
    }

    private static String shortUuid(String uuid) {
        String u = uuid.toLowerCase(java.util.Locale.US);
        int i = u.indexOf("0000");
        if (u.startsWith("0000") && u.contains("-0000-1000-8000-00805f9b34fb")) {
            return u.substring(4, 8);
        }
        return u.length() > 12 ? u.substring(0, 12) + "…" : u;
    }

    private String safeName(BluetoothDevice d) {
        try {
            String n = d.getName();
            return n == null || n.isEmpty() ? d.getAddress() : n;
        } catch (SecurityException e) {
            return "未知设备";
        }
    }

    /* ------------------------------ 发送：键盘 ------------------------------ */

    public void pressKey(int usage) {
        if (keyboard.keyDown(usage)) sendKeyboard();
    }

    public void releaseKey(int usage) {
        if (keyboard.keyUp(usage)) sendKeyboard();
    }

    public void setModifier(int modIndex, boolean down) {
        // 状态没变就不占用一条 L2CAP 报告
        if (keyboard.isModifierDown(modIndex) == down) return;
        keyboard.setModifier(modIndex, down);
        sendKeyboard();
    }

    public void releaseAllModifiers() {
        keyboard.clearModifiers();
        sendKeyboard();
    }

    /** 单击：按下后延迟抬起，保证主机能观察到完整行程。 */
    public void tapKey(int usage) {
        if (!keyboard.keyDown(usage)) return;
        sendKeyboard();
        worker.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (keyboard.keyUp(usage)) sendKeyboard();
            }
        }, TAP_GAP_MS);
    }

    /** 组合键，例如 Ctrl+Alt+Del：整组按下后统一抬起。 */
    public void sendChord(int modifierBits, int[] usages) {
        for (int i = 0; i < 8; i++) {
            if ((modifierBits & HidUsages.MOD_BIT[i]) != 0) keyboard.setModifier(i, true);
        }
        for (int u : usages) keyboard.keyDown(u);
        sendKeyboard();
        worker.postDelayed(new Runnable() {
            @Override
            public void run() {
                for (int i = 0; i < 8; i++) keyboard.setModifier(i, false);
                keyboard.releaseAll();
                sendKeyboard();
            }
        }, 24);
    }

    public void sendKeyboard() {
        sendReport(HidDescriptor.REPORT_ID_KEYBOARD, keyboard.toReport());
    }

    /* ------------------------------ 发送：鼠标 ------------------------------ */

    /**
     * 提交一次相对位移。dx/dy 是屏幕像素，dtMs 用于指针加速。
     */
    public void moveBy(float dx, float dy, float dtMs) {
        int[] step = pointer.offer(dx, dy, dtMs);
        if (step[0] == 0 && step[1] == 0) return;
        enqueueMouse(step[0], step[1], 0);
    }

    /** 提交滚轮位移（像素），内部自动量化成棘轮步数。 */
    public void wheelBy(float dyPx) {
        int steps = scroll.offer(dyPx);
        if (steps != 0) enqueueMouse(0, 0, steps);
    }

    public int mouseButtons() {
        return mouseButtons;
    }

    public void setMouseButtons(int mask) {
        if (mask == mouseButtons) return;
        mouseButtons = mask;
        worker.post(new Runnable() {
            @Override
            public void run() {
                sendMouseState();
            }
        });
    }

    /** 立刻发一帧"当前按键状态"的鼠标报告（位移为零）。只在 worker 线程调用。 */
    private void sendMouseState() {
        int buttons = mouseButtons;
        sendReport(HidDescriptor.REPORT_ID_MOUSE,
                new byte[]{(byte) buttons, (byte) 0, (byte) 0, (byte) 0});
        lastSentButtons = buttons;
    }

    /** 单击某个鼠标键 */
    public void tapMouseButton(final int mask) {
        final int before = mouseButtons;
        mouseButtons = before | mask;
        worker.post(new Runnable() {
            @Override
            public void run() {
                sendMouseState();          // 按下
            }
        });
        worker.postDelayed(new Runnable() {
            @Override
            public void run() {
                mouseButtons = before & ~mask;
                sendMouseState();          // 抬起
            }
        }, 30);
    }

    /** 横滚暂不支持（内置描述符无 AC Pan），用 Shift+滚轮 近似在调用方处理。 */
    public void enqueueMouse(int dx, int dy, int wheel) {
        synchronized (mouseLock) {
            accX += dx;
            accY += dy;
            accWheel += wheel;
        }
        scheduleFlush();
    }

    private void scheduleFlush() {
        synchronized (mouseLock) {
            if (flushScheduled) return;
            flushScheduled = true;
        }
        worker.postDelayed(flushRunnable, MOUSE_FLUSH_MS);
    }

    private final Runnable flushRunnable = new Runnable() {
        @Override
        public void run() {
            int dx;
            int dy;
            int wh;
            synchronized (mouseLock) {
                dx = accX;
                dy = accY;
                wh = accWheel;
                accX = 0;
                accY = 0;
                accWheel = 0;
                flushScheduled = false;
            }
            int guard = 0;
            // 关键：mouseButtons != lastSentButtons 表示"只按了键、没动"，同样要发一帧，
            // 否则单击/拖拽的按下与抬起会被这里整个吞掉（旧版就是这么丢点击的）。
            while ((dx != 0 || dy != 0 || wh != 0 || mouseButtons != lastSentButtons) && guard++ < 8) {
                int rx = clamp8(dx);
                int ry = clamp8(dy);
                int rw = clamp8(wh);
                int buttons = mouseButtons;
                byte[] report = new byte[]{
                        (byte) buttons, (byte) rx, (byte) ry, (byte) rw
                };
                if (!sendReport(HidDescriptor.REPORT_ID_MOUSE, report)) break;
                lastSentButtons = buttons;
                dx -= rx;
                dy -= ry;
                wh -= rw;
                if (dx == 0 && dy == 0 && wh == 0 && buttons == mouseButtons) break;
            }
            boolean more;
            synchronized (mouseLock) {
                more = accX != 0 || accY != 0 || accWheel != 0;
            }
            if (more) scheduleFlush();
        }
    };

    /* ------------------------------ 发送：消费类 ------------------------------ */

    public void sendConsumer(final int usage) {
        sendReport(HidDescriptor.REPORT_ID_CONSUMER, consumerBytes(usage));
        worker.postDelayed(new Runnable() {
            @Override
            public void run() {
                sendReport(HidDescriptor.REPORT_ID_CONSUMER, consumerBytes(0));
            }
        }, 14);
    }

    private static byte[] consumerBytes(int usage) {
        return new byte[]{(byte) (usage & 0xFF), (byte) ((usage >> 8) & 0xFF)};
    }

    /* ------------------------------ 底层发送 ------------------------------ */

    private boolean sendReport(int id, byte[] data) {
        BluetoothHidDevice h = hid;
        BluetoothDevice d = host;
        if (h == null || d == null || !connected) return false;
        try {
            return h.sendReport(d, id, data);
        } catch (SecurityException e) {
            Log.w(TAG, "sendReport 权限不足", e);
            return false;
        } catch (Exception e) {
            Log.w(TAG, "sendReport failed id=" + id, e);
            return false;
        }
    }

    private static int clamp8(int v) {
        if (v > 127) return 127;
        if (v < -127) return -127;
        return v;
    }

    /* ------------------------------ 回包 ------------------------------ */

    private byte[] reportFor(int id) {
        switch (id) {
            case HidDescriptor.REPORT_ID_KEYBOARD:
                return keyboard.toReport();
            case HidDescriptor.REPORT_ID_MOUSE:
                return new byte[]{(byte) mouseButtons, 0, 0, 0};
            case HidDescriptor.REPORT_ID_CONSUMER:
                return new byte[2];
            default:
                return null;
        }
    }

    private final BluetoothHidDevice.Callback callback = new BluetoothHidDevice.Callback() {
        @Override
        public void onAppStatusChanged(BluetoothDevice pluggedDevice, boolean isRegistered) {
            statusCallbackSeen = true;
            registered = isRegistered;
            event("onAppStatusChanged registered=" + isRegistered);
            if (isRegistered) {
                lastRegisterError = "";
                notifyState(true, "已就绪：请到电脑上「添加蓝牙设备」选择 SwitchPad");
                reconcileConnection();
            } else if (wantRegistered && !connected) {
                // 系统"受理了注册请求但最终没注册上"：多半是栈里残留，退避后重来
                lastRegisterError = "系统受理后仍未注册成功（onAppStatusChanged=false）";
                notifyState(false, "系统未完成注册，稍后重试…");
                if (registerAttempts < MAX_REGISTER_ATTEMPTS * 2) {
                    worker.postDelayed(registerRetry, RETRY_DELAY_MS);
                }
            } else {
                notifyState(false, "HID 应用已注销");
            }
        }

        @Override
        public void onConnectionStateChanged(BluetoothDevice device, int state) {
            String who = safeName(device);
            switch (state) {
                case BluetoothHidDevice.STATE_CONNECTED:
                    connecting = false;
                    host = device;
                    hostName = who;
                    connected = true;
                    event("CONNECTED " + who + " · " + linkType());
                    notifyState(true, "已连接：" + hostName);
                    notifyConnection(true, hostName);
                    break;
                case BluetoothHidDevice.STATE_CONNECTING:
                    connecting = true;
                    connectStartedAt = android.os.SystemClock.elapsedRealtime();
                    event("CONNECTING " + who);
                    notifyState(registered, "正在连接 " + who + " …");
                    break;
                case BluetoothHidDevice.STATE_DISCONNECTING:
                    event("DISCONNECTING " + who);
                    break;
                case BluetoothHidDevice.STATE_DISCONNECTED:
                default:
                    if (connected) event("DISCONNECTED " + who + "（曾连上后断开）");
                    else event("DISCONNECTED " + who + "（未连上即失败）");
                    connecting = false;
                    lastConnectFailAt = android.os.SystemClock.elapsedRealtime();
                    connected = false;
                    host = null;
                    keyboard.releaseAll();
                    mouseButtons = 0;
                    lastSentButtons = 0;
                    notifyConnection(false, hostName);
                    notifyState(registered, registered
                            ? "已断开，等待重新配对/连接（可点「再次连接」）" : "已断开");
                    break;
            }
        }

        @Override
        public void onGetReport(BluetoothDevice device, byte type, byte id, int bufferSize) {
            int rid = id & 0xFF;
            BluetoothHidDevice h = hid;
            if (h == null) return;
            event("GET_REPORT type=" + (type & 0xFF) + " id=" + rid
                    + " proto=" + ((currentProtocol & 0xFF) == 0 ? "boot" : "report"));
            try {
                byte[] data = reportFor(rid);
                if (data == null) {
                    // 主机在 Boot 协议下会用 id=0 索取输入报告；这里绝不能回 HANDSHAKE 错误，
                    // 否则 Windows 会把这个 HID 设备判成坏的并主动断开。
                    if (rid == 0) {
                        data = keyboard.toReport();
                    } else {
                        data = new byte[Math.max(1, Math.min(16, bufferSize))];
                    }
                }
                h.replyReport(device, type, (byte) rid, data);
            } catch (Exception e) {
                Log.w(TAG, "onGetReport", e);
                event("GET_REPORT 回复失败：" + e);
            }
        }

        @Override
        public void onSetReport(BluetoothDevice device, byte type, byte id, byte[] data) {
            if ((id & 0xFF) == HidDescriptor.REPORT_ID_KEYBOARD && data != null && data.length > 0) {
                leds = data[0] & 0xFF;
                notifyLeds();
            }
        }

        @Override
        public void onSetProtocol(BluetoothDevice device, byte protocol) {
            currentProtocol = protocol;
        }

        @Override
        public void onInterruptData(BluetoothDevice device, byte reportId, byte[] data) {
            // 本项目不需要接收主机下发的数据
        }

        @Override
        public void onVirtualCableUnplug(BluetoothDevice device) {
            connected = false;
            host = null;
            notifyConnection(false, hostName);
            notifyState(registered, "电脑主动解除了连接");
        }
    };

    /* ------------------------------ 监听器 ------------------------------ */

    public void addListener(Listener l) {
        if (l != null && !listeners.contains(l)) listeners.add(l);
    }

    public void removeListener(Listener l) {
        listeners.remove(l);
    }

    private void notifyState(final boolean reg, final String message) {
        Log.i(TAG, "state: " + message);
        lastStateMessage = message;
        main.post(new Runnable() {
            @Override
            public void run() {
                for (Listener l : listeners) l.onHidState(reg, message);
            }
        });
    }

    private void notifyConnection(final boolean conn, final String name) {
        main.post(new Runnable() {
            @Override
            public void run() {
                for (Listener l : listeners) l.onConnection(conn, name == null ? "" : name);
            }
        });
    }

    private void notifyLeds() {
        final int bits = leds;
        main.post(new Runnable() {
            @Override
            public void run() {
                for (Listener l : listeners) l.onLeds(bits);
            }
        });
    }
}
