package com.paopao.switchkey;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.List;

/**
 * 主界面：一行紧凑状态条 + 整屏键盘/触控板。
 * 次要操作（启动、重连、配对被发现、诊断、兼容模式）都收进右上角「⋯」菜单，
 * 好把纵向空间尽量留给键盘。
 */
public class MainActivity extends Activity implements HidKeyboard.Listener, BleHidServer.Listener {

    private static final int REQ_PERM = 1001;

    private TextView titleView;
    private TextView modeBtn;
    private TextView statusLine;
    private TextView diagView;
    private View diagBox;
    private ScrollView diagScroll;
    private KeyboardView keyboardView;
    private MousePadView mousePad;
    private HidKeyboard keyboard;
    private BleHidServer bleHid;

    private boolean autoStarted;
    private boolean mouseMode;
    private boolean useBle;
    private boolean pendingDiscoverable;
    private String statusDetail = "";
    private final List<String> diagLines = new ArrayList<>();

    private Handler mainHandler;
    private PointerPump pointerPump;
    private int pointerRate = PointerPump.RATE_60HZ;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setContentView(buildUi());

        keyboard = HidKeyboard.get(this);
        keyboard.setListener(this);
        bleHid = new BleHidServer(this);
        bleHid.setListener(this);

        // 指针报告合并：Switch 的蓝牙在 115200 bps 串口上，不能每个触摸事件都发一帧
        mainHandler = new Handler(Looper.getMainLooper());
        pointerPump = new PointerPump(new PointerPump.Sink() {
            @Override
            public void onMouse(int buttons, int dx, int dy, int wheel) {
                if (useBle) {
                    bleHid.sendMouse(buttons, dx, dy, wheel);
                } else {
                    keyboard.sendMouse(buttons, dx, dy, wheel);
                }
            }
        }, new PointerPump.Clock() {
            @Override
            public long now() {
                return SystemClock.uptimeMillis();
            }

            @Override
            public void postDelayed(Runnable r, long delayMs) {
                mainHandler.postDelayed(r, delayMs);
            }

            @Override
            public void cancel(Runnable r) {
                mainHandler.removeCallbacks(r);
            }
        });
        pointerPump.setIntervalMs(pointerRate);
        keyboardView.setSink(new KeyboardView.ReportSink() {
            @Override
            public void onReport(int modifiers, int[] usages, int count) {
                if (useBle) {
                    bleHid.send(modifiers, usages, count);
                } else {
                    keyboard.send(modifiers, usages, count);
                }
            }
        });
        mousePad.setSink(new MousePadView.MouseSink() {
            @Override
            public void onMouse(int buttons, int dx, int dy, int wheel) {
                pointerPump.onEvent(buttons, dx, dy, wheel);
            }
        });
        onDiag("SwitchKey v1.6 启动（传输：" + (useBle ? "BLE" : "经典 HID") + "）");
    }

    @Override
    protected void onResume() {
        super.onResume();
        keyboard.setListener(useBle ? null : this);
        bleHid.setListener(useBle ? this : null);
        if (!autoStarted) {
            if (hasPermissions()) {
                autoStarted = true;
                startActiveTransport();
            } else {
                requestPermissions();
            }
        }
    }

    private void startActiveTransport() {
        if (useBle) {
            bleHid.setKeyboardOnly(keyboard.isKeyboardOnly());
            bleHid.start();
        } else {
            keyboard.start();
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        keyboardView.releaseAll();
        mousePad.releaseAll();
        pointerPump.flush();
    }

    @Override
    protected void onDestroy() {
        keyboard.setListener(null);
        bleHid.setListener(null);
        super.onDestroy();
    }

    // ---------------- UI ----------------

    private View buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xFF14161A);
        root.setFitsSystemWindows(true);

        int hp = dp(10);

        // ---- 一行状态条：状态 + 模式切换 + 更多 ----
        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(hp, dp(5), hp, dp(5));
        header.setBackgroundColor(0xFF1B1F26);

        titleView = new TextView(this);
        titleView.setText("正在初始化…");
        titleView.setTextColor(0xFFE9ECF1);
        titleView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        titleView.setMaxLines(1);
        titleView.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showMenu();
            }
        });
        header.addView(titleView, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        modeBtn = pill("切到鼠标", 0xFF2F6FE0, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                setMouseMode(!mouseMode);
            }
        });
        header.addView(modeBtn, pillParams());

        TextView moreBtn = pill("⋯", 0xFF3B4250, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showMenu();
            }
        });
        LinearLayout.LayoutParams mp = pillParams();
        mp.leftMargin = dp(6);
        header.addView(moreBtn, mp);

        root.addView(header, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        // ---- 诊断面板（默认隐藏，占高度时会挤压键盘） ----
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setBackgroundColor(0xFF101317);

        LinearLayout diagTools = new LinearLayout(this);
        diagTools.setOrientation(LinearLayout.HORIZONTAL);
        diagTools.setPadding(hp, dp(6), hp, dp(2));

        TextView compatBtn = pill("仅键盘兼容模式", 0xFF7A4A12, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                toggleCompat();
            }
        });
        diagTools.addView(compatBtn, pillParams());

        TextView copyBtn = pill("复制日志", 0xFF3B4250, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                copyDiag();
            }
        });
        LinearLayout.LayoutParams cp = pillParams();
        cp.leftMargin = dp(6);
        diagTools.addView(copyBtn, cp);
        box.addView(diagTools);

        diagView = new TextView(this);
        diagView.setTextColor(0xFF9FB2C8);
        diagView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 9);
        diagView.setTypeface(Typeface.MONOSPACE);
        diagView.setPadding(hp, 0, hp, dp(6));

        diagScroll = new ScrollView(this);
        diagScroll.addView(diagView);
        box.addView(diagScroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        box.setVisibility(View.GONE);
        diagBox = box;
        root.addView(box, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 2f));

        // ---- 键盘 / 触控板（互斥显示，占满剩余高度） ----
        keyboardView = new KeyboardView(this);
        root.addView(keyboardView, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        mousePad = new MousePadView(this);
        mousePad.setVisibility(View.GONE);
        root.addView(mousePad, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        // ---- 底部一行状态说明（替代原来占一整行的第二行） ----
        statusLine = new TextView(this);
        statusLine.setTextColor(0xFF6C737D);
        statusLine.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10);
        statusLine.setGravity(Gravity.CENTER);
        statusLine.setMaxLines(1);
        statusLine.setPadding(hp, dp(3), hp, dp(4));
        root.addView(statusLine, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        setMouseMode(false);
        return root;
    }

    private void setMouseMode(boolean mouse) {
        mouseMode = mouse;
        if (mouse) {
            keyboardView.releaseAll();
            keyboardView.setVisibility(View.GONE);
            mousePad.setVisibility(View.VISIBLE);
            modeBtn.setText("切到键盘");
        } else {
            mousePad.releaseAll();
            if (pointerPump != null) {
                pointerPump.flush(); // buildUi() 里会先调一次，此时 pump 还没建
            }
            mousePad.setVisibility(View.GONE);
            keyboardView.setVisibility(View.VISIBLE);
            modeBtn.setText("切到鼠标");
        }
        updateStatusLine();
    }

    private void updateStatusLine() {
        if (statusDetail != null && statusDetail.length() > 0) {
            statusLine.setText(statusDetail);
        } else if (mouseMode) {
            statusLine.setText("拖动=移动　轻点=左键　两指轻点=右键　两指拖动=滚轮　·　" + rateLabel());
        } else {
            statusLine.setText("Shift / Ctrl / Alt：轻点=一次性，双击=锁定　·　中文用电脑输入法打拼音");
        }
    }

    private void setDiagVisible(boolean visible) {
        diagBox.setVisibility(visible ? View.VISIBLE : View.GONE);
    }

    private void toggleCompat() {
        boolean next = !keyboard.isKeyboardOnly();
        keyboard.setKeyboardOnly(next);
        bleHid.setKeyboardOnly(next);
        toast(next
                ? "已切到「仅键盘」：请在 Windows 删除设备后重新配对"
                : "已切回「键盘+鼠标」：请在 Windows 删除设备后重新配对");
    }

    private String rateLabel() {
        switch (pointerRate) {
            case PointerPump.RATE_60HZ:
                return "60Hz";
            case PointerPump.RATE_30HZ:
                return "30Hz 省链路";
            default:
                return "不限速";
        }
    }

    /**
     * 60Hz -> 30Hz -> 不限速 -> 60Hz。
     * 蓝牙 UART 还是 115200 bps（没打 hcd 补丁、没开 3Mbaud）时，30Hz 明显更稳。
     */
    private void cyclePointerRate() {
        if (pointerRate == PointerPump.RATE_60HZ) {
            pointerRate = PointerPump.RATE_30HZ;
        } else if (pointerRate == PointerPump.RATE_30HZ) {
            pointerRate = PointerPump.RATE_OFF;
        } else {
            pointerRate = PointerPump.RATE_60HZ;
        }
        pointerPump.setIntervalMs(pointerRate);
        toast("指针发送速率：" + rateLabel());
        onDiag("指针速率 -> " + rateLabel());
        updateStatusLine();
    }

    private void copyDiag() {
        StringBuilder sb = new StringBuilder();
        for (String s : diagLines) {
            sb.append(s).append('\n');
        }
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (cm != null) {
            cm.setPrimaryClip(ClipData.newPlainText("SwitchKey log", sb.toString()));
            toast("诊断日志已复制");
        }
    }

    private void showMenu() {
        final String[] items = new String[] {
                "启动键盘 / 重新注册",
                useBle ? "传输：BLE（HOGP）→ 切到经典 HID"
                        : "传输：经典 HID → 切到 BLE（HOGP）",
                "连接已配对的电脑（推一次连接 · 仅经典）",
                "重连上次的电脑（仅经典）",
                "让手机可被发现（配对 · 仅经典）",
                "断开当前 BLE 连接（" + (useBle ? bleHid.getHostLabel() : "-") + "）",
                (bleHid.isManualCccd() ? "BLE: CCCD=手动 → 改成系统自动"
                        : "BLE: CCCD=自动 → 改成手动补"),
                (diagBox.getVisibility() == View.VISIBLE ? "隐藏" : "显示") + "诊断日志",
                (keyboard.isKeyboardOnly() ? "关闭" : "开启") + "仅键盘兼容模式",
                "打开手机蓝牙设置（清理配对）",
                "复制诊断日志",
                "指针速率：" + rateLabel() + " → 切换",
        };
        new AlertDialog.Builder(this)
                .setTitle("SwitchKey　手机蓝牙名：" + keyboard.getAdapterName()
                        + "　当前：" + (useBle ? "BLE" : "经典"))
                .setItems(items, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        switch (which) {
                            case 0:
                                if (!hasPermissions()) {
                                    requestPermissions();
                                } else if (useBle) {
                                    bleHid.start();
                                } else {
                                    keyboard.start();
                                }
                                break;
                            case 1:
                                setTransport(!useBle);
                                break;
                            case 2:
                                if (useBle) {
                                    toast("BLE 模式只能由电脑发起连接：请在 Windows 里点这个设备");
                                } else {
                                    showBondedDialog();
                                }
                                break;
                            case 3:
                                if (useBle) {
                                    toast("BLE 模式只能由电脑发起连接，手机不能主动连");
                                } else if (!keyboard.hasRememberedHost()) {
                                    toast("还没配对过电脑，先用「让手机可被发现」");
                                } else {
                                    toast("正在重连上次的电脑…");
                                    keyboard.reconnectNow();
                                }
                                break;
                            case 4:
                                makeDiscoverable();
                                break;
                            case 5:
                                bleHid.disconnectHost();
                                toast("已请求断开 BLE 连接");
                                break;
                            case 6:
                                bleHid.setManualCccd(!bleHid.isManualCccd());
                                toast(bleHid.isManualCccd()
                                        ? "BLE 已改为手动补 CCCD，正在重建服务"
                                        : "BLE 已改回系统自动补 CCCD，正在重建服务");
                                break;
                            case 7:
                                setDiagVisible(diagBox.getVisibility() != View.VISIBLE);
                                break;
                            case 8:
                                toggleCompat();
                                break;
                            case 9:
                                openBluetoothSettings();
                                break;
                            case 10:
                                copyDiag();
                                break;
                            case 11:
                                cyclePointerRate();
                                break;
                            default:
                                break;
                        }
                    }
                })
                .setNegativeButton("关闭", null)
                .show();
    }

    /** 经典 HID 与 BLE HOGP 二选一，同一时间只跑一个 */
    private void setTransport(boolean ble) {
        if (useBle == ble) {
            return;
        }
        useBle = ble;
        keyboardView.releaseAll();
        mousePad.releaseAll();
        bleHid.setKeyboardOnly(keyboard.isKeyboardOnly());
        if (ble) {
            keyboard.setListener(null);
            keyboard.stop();
            keyboard.setListener(this);
            bleHid.setListener(null);
            bleHid.setListener(this);
            bleHid.start();
            toast("已切到 BLE HOGP：Windows 里添加设备找「" + bleHid.getAdapterName() + "」");
        } else {
            bleHid.stop();
            keyboard.start();
            toast("已切回经典 HID：点 ⋯ →「让手机可被发现」去配对");
        }
        onDiag("传输切换为 " + (ble ? "BLE" : "经典"));
    }

    /** 列出手机上已配对的设备，手动推一次 HID 连接 */
    private void showBondedDialog() {
        final List<BluetoothDevice> bonded = keyboard.bondedDevices();
        if (bonded.isEmpty()) {
            toast("手机上还没有已配对设备：先去 Windows 配对一次");
            return;
        }
        String[] names = new String[bonded.size()];
        for (int i = 0; i < bonded.size(); i++) {
            names[i] = keyboard.deviceLabel(bonded.get(i));
        }
        new AlertDialog.Builder(this)
                .setTitle("推一次连接（选电脑）")
                .setItems(names, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        keyboard.connectTo(bonded.get(which));
                        toast("已请求连接，看状态条变化");
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void openBluetoothSettings() {
        try {
            startActivity(new Intent(android.provider.Settings.ACTION_BLUETOOTH_SETTINGS));
        } catch (Exception e) {
            toast("打不开蓝牙设置，请手动进入系统设置");
        }
    }

    private TextView pill(String text, int bg, View.OnClickListener l) {
        TextView tv = new TextView(this);
        tv.setText(text);
        tv.setTextColor(Color.WHITE);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        tv.setGravity(Gravity.CENTER);
        tv.setPadding(dp(12), dp(5), dp(12), dp(5));
        GradientDrawable bgd = new GradientDrawable();
        bgd.setColor(bg);
        bgd.setCornerRadius(dp(16));
        tv.setBackground(bgd);
        tv.setOnClickListener(l);
        return tv;
    }

    private LinearLayout.LayoutParams pillParams() {
        return new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private int dp(float v) {
        return (int) (v * getResources().getDisplayMetrics().density + 0.5f);
    }

    // ---------------- 权限 ----------------

    private String[] neededPermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                return new String[] {
                        Manifest.permission.BLUETOOTH_CONNECT,
                        Manifest.permission.BLUETOOTH_ADVERTISE,
                        Manifest.permission.POST_NOTIFICATIONS
                };
            }
            return new String[] {
                    Manifest.permission.BLUETOOTH_CONNECT,
                    Manifest.permission.BLUETOOTH_ADVERTISE
            };
        }
        return new String[0];
    }

    private boolean hasPermissions() {
        for (String p : neededPermissions()) {
            if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) {
                return false;
            }
        }
        return true;
    }

    private void requestPermissions() {
        String[] perms = neededPermissions();
        if (perms.length == 0) {
            startActiveTransport();
            return;
        }
        requestPermissions(perms, REQ_PERM);
    }

    @Override
    public void onRequestPermissionsResult(int req, String[] perms, int[] results) {
        super.onRequestPermissionsResult(req, perms, results);
        if (req != REQ_PERM) {
            return;
        }
        autoStarted = true;
        boolean btOk = true;
        for (int i = 0; i < perms.length; i++) {
            if (Manifest.permission.BLUETOOTH_CONNECT.equals(perms[i])
                    && results[i] != PackageManager.PERMISSION_GRANTED) {
                btOk = false;
            }
        }
        if (btOk) {
            startActiveTransport();
        } else {
            onKeyboardState(HidKeyboard.STATE_ERROR, "缺少蓝牙权限",
                    "请在系统设置里允许「附近的设备」权限");
        }
    }

    // ---------------- 配对 ----------------

    /**
     * 关键：必须等 registerApp 真的成功（状态变 READY）再让手机可被发现。
     * 否则 Windows 扫到的是一个没有 HID 服务的空壳，配对后会落在「其他设备」里，打不出字。
     */
    private void makeDiscoverable() {
        if (useBle) {
            toast("BLE 模式一直在广播，直接去 Windows「添加设备」找「"
                    + bleHid.getAdapterName() + "」即可");
            return;
        }
        BluetoothAdapter adapter = keyboard.getAdapter();
        if (adapter == null || !adapter.isEnabled()) {
            toast("请先打开手机蓝牙");
            return;
        }
        if (!keyboard.isRegistered()) {
            pendingDiscoverable = true;
            toast("正在注册蓝牙键盘服务，成功后会弹出「可被发现」");
            keyboard.start();
            return;
        }
        launchDiscoverable();
    }

    private void launchDiscoverable() {
        pendingDiscoverable = false;
        boolean launched;
        try {
            Intent i = new Intent(BluetoothAdapter.ACTION_REQUEST_DISCOVERABLE);
            i.putExtra(BluetoothAdapter.EXTRA_DISCOVERABLE_DURATION, 0);
            startActivity(i);
            launched = true;
        } catch (Exception e) {
            launched = false;
            onDiag("打开可被发现界面失败: " + e);
        }
        String name = keyboard.getAdapterName();
        toast(launched
                ? "在 Windows 里找「" + name + "」（不是你手机型号名，是蓝牙名）→ 添加设备"
                : "请手动在系统设置里把蓝牙设为「可被发现」");
    }

    private void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_LONG).show();
    }

    // ---------------- 状态回调 ----------------

    @Override
    public void onKeyboardState(int state, String title, String detail) {
        titleView.setText(title);
        statusDetail = detail == null ? "" : detail;
        int color;
        switch (state) {
            case HidKeyboard.STATE_CONNECTED:
                color = 0xFF4CD964;
                break;
            case HidKeyboard.STATE_READY:
                color = 0xFFFFC53D;
                break;
            case HidKeyboard.STATE_UNSUPPORTED:
            case HidKeyboard.STATE_ERROR:
                color = 0xFFFF5F56;
                break;
            default:
                color = 0xFFE9ECF1;
                break;
        }
        titleView.setTextColor(color);
        updateStatusLine();

        if (state == HidKeyboard.STATE_READY && pendingDiscoverable) {
            launchDiscoverable();
        }
    }

    @Override
    public void onDiag(String line) {
        diagLines.add(line);
        while (diagLines.size() > 200) {
            diagLines.remove(0);
        }
        StringBuilder sb = new StringBuilder();
        for (String s : diagLines) {
            sb.append(s).append('\n');
        }
        diagView.setText(sb.toString());
        diagScroll.post(new Runnable() {
            @Override
            public void run() {
                diagScroll.fullScroll(View.FOCUS_DOWN);
            }
        });
    }

    // ---------------- BLE HOGP 状态回调 ----------------

    @Override
    public void onBleState(int state, String title, String detail) {
        titleView.setText(title);
        statusDetail = detail == null ? "" : detail;
        int color;
        if (state == BleHidServer.STATE_CONNECTED) {
            color = 0xFF4CD964;
        } else if (state == BleHidServer.STATE_ADVERTISING) {
            color = 0xFFFFC53D;
        } else if (state == BleHidServer.STATE_ERROR) {
            color = 0xFFFF5F56;
        } else {
            color = 0xFFE9ECF1;
        }
        titleView.setTextColor(color);
        updateStatusLine();
    }

    @Override
    public void onBleDiag(String line) {
        onDiag("[BLE] " + line);
    }
}
