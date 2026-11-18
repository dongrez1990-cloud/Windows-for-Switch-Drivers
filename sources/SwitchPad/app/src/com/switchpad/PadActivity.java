package com.switchpad;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.CompoundButton;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

import java.util.List;

/**
 * 主界面：上方是触控板，下方是屏幕键盘，未连接时弹出配对面板。
 *
 * 整个 App 没有第二个界面，也不需要服务端程序：
 * 电脑（跑 Windows 11 ARM 的 Switch）把它当成一只普通的蓝牙键鼠。
 */
public final class PadActivity extends Activity
        implements HidCore.Listener, TouchpadView.PadBridge, KeyPadView.HidBridge,
        HidCore.ScanListener {

    private static final int REQ_PERMS = 0x51;
    private static final int REQ_ENABLE_BT = 0x52;
    private static final int REQ_DISCOVERABLE = 0x53;

    private HidCore core;

    private TextView statusText;
    private TextView ledText;
    private TouchpadView pad;
    private KeyPadView keys;
    private LinearLayout contentRoot;
    private View connectPanel;
    private TextView panelStatus;
    private TextView panelHint;
    private TextView diagText;
    private TextView scanStatus;
    private LinearLayout scanList;
    private final java.util.LinkedHashMap<String, Button> scanRows =
            new java.util.LinkedHashMap<String, Button>();
    private Button discoverBtn;
    private LinearLayout bondedList;
    private Button modeBtn;

    /** 布局三档：分屏 / 仅触控板 / 仅键盘 */
    private static final int MODE_SPLIT = 0;
    private static final int MODE_PAD = 1;
    private static final int MODE_KEYS = 2;
    private static final String[] MODE_LABEL = {"分屏", "触控", "键盘"};
    private int mode = MODE_SPLIT;
    private boolean started;

    /* ------------------------------------------------------------------ */

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        hideSystemBars();

        core = HidCore.get(this);
        setContentView(buildUi());
        core.addListener(this);
        core.addScanListener(this);
        core.registerReceivers();

        // 恢复上次的布局：分屏 / 仅触控板 / 仅键盘
        mode = Prefs.layoutMode(this);
        applyMode();

        requestBlePermissions();
    }

    private void hideSystemBars() {
        View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
        core.applySettings();
        pad.setHaptics(Prefs.haptics(this));
        keys.setHaptics(Prefs.haptics(this));
        if (hasBtPermission()) startHudIfPossible();
        refreshStatus();
        rebuildBondedList();
    }

    @Override
    protected void onDestroy() {
        core.removeListener(this);
        core.removeScanListener(this);
        super.onDestroy();
    }

    /* ------------------------------ 权限 ------------------------------ */

    private boolean hasBtPermission() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return true;
        return checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED;
    }

    private void requestBlePermissions() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            startHudIfPossible();
            return;
        }
        String[] perms = new String[]{
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.POST_NOTIFICATIONS
        };
        boolean need = false;
        for (String p : perms) {
            if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) need = true;
        }
        if (!need) {
            startHudIfPossible();
        } else {
            requestPermissions(perms, REQ_PERMS);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_PERMS) {
            if (hasBtPermission()) {
                startHudIfPossible();
            } else {
                panelStatus.setText("缺少「附近的设备」权限，无法使用蓝牙键鼠");
                panelStatus.setTextColor(Ui.ERR);
            }
        }
    }

    /* ------------------------------ 启动 HID ------------------------------ */

    private void startHudIfPossible() {
        if (started) return;
        BluetoothAdapter adapter = core.adapter();
        if (adapter == null) {
            panelStatus.setText("本机没有蓝牙适配器");
            panelStatus.setTextColor(Ui.ERR);
            return;
        }
        if (!adapter.isEnabled()) {
            panelStatus.setText("蓝牙未开启，请先打开蓝牙");
            panelStatus.setTextColor(Ui.WARN);
            return;
        }
        started = true;
        try {
            startForegroundService(new Intent(this, HidService.class));
        } catch (Exception e) {
            // 退化为普通启动，HID 逻辑仍然可用
            startService(new Intent(this, HidService.class));
        }
        core.start();
    }

    /* ------------------------------ 界面 ------------------------------ */

    private View buildUi() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Ui.BG);

        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        root.addView(column, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        contentRoot = column;

        /* --- 状态条 --- */
        LinearLayout statusRow = new LinearLayout(this);
        statusRow.setOrientation(LinearLayout.HORIZONTAL);
        statusRow.setGravity(Gravity.CENTER_VERTICAL);
        statusRow.setPadding(Ui.dp(this, 12), Ui.dp(this, 6), Ui.dp(this, 8), Ui.dp(this, 2));

        statusText = Ui.text(this, "未连接", Ui.TEXT_DIM, 12.5f);
        statusText.setSingleLine(true);
        statusRow.addView(statusText, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        ledText = Ui.text(this, "", Ui.TEXT_DIM, 11f);
        statusRow.addView(ledText, Ui.wrap());

        Button settingsBtn = Ui.button(this, "⚙", Ui.PANEL_2, Ui.TEXT, 14);
        settingsBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showSettings();
            }
        });
        LinearLayout.LayoutParams slp = Ui.wrap();
        slp.leftMargin = Ui.dp(this, 8);
        statusRow.addView(settingsBtn, slp);

        // 长按状态条也能切换布局（动作条上的按钮太小、手指又常在上面）
        statusRow.setOnLongClickListener(new View.OnLongClickListener() {
            @Override
            public boolean onLongClick(View v) {
                setMode((mode + 1) % 3);
                toast("布局：" + MODE_LABEL[mode]);
                return true;
            }
        });
        column.addView(statusRow);

        /* --- 触控板 --- */
        pad = new TouchpadView(this);
        pad.setBridge(this);
        column.addView(pad, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1.0f));

        /* --- 快捷动作条 --- */
        column.addView(buildActionRow());

        /* --- 屏幕键盘 --- */
        keys = new KeyPadView(this);
        keys.setBridge(this);
        column.addView(keys, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1.15f));

        root.addView(buildConnectPanel(),
                new FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        return root;
    }

    private View buildActionRow() {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER);
        row.setPadding(Ui.dp(this, 8), Ui.dp(this, 2), Ui.dp(this, 8), Ui.dp(this, 2));
        row.setLayoutParams(new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, Ui.dp(this, 46)));

        row.addView(holdButton("左键", HidUsages.BTN_LEFT));
        row.addView(holdButton("中键", HidUsages.BTN_MIDDLE));
        row.addView(holdButton("右键", HidUsages.BTN_RIGHT));

        row.addView(actionButton("C+A\nDel", new Runnable() {
            @Override
            public void run() {
                core.sendChord(HidUsages.MOD_BIT[HidUsages.MOD_LCTRL]
                        | HidUsages.MOD_BIT[HidUsages.MOD_LALT], new int[]{HidUsages.K_DELETE});
            }
        }));
        row.addView(actionButton("Alt\nTab", new Runnable() {
            @Override
            public void run() {
                core.sendChord(HidUsages.MOD_BIT[HidUsages.MOD_LALT], new int[]{HidUsages.K_TAB});
            }
        }));
        row.addView(actionButton("⊞\nD", new Runnable() {
            @Override
            public void run() {
                core.sendChord(HidUsages.MOD_BIT[HidUsages.MOD_LGUI], new int[]{HidUsages.letter('d')});
            }
        }));
        row.addView(actionButton("Esc", new Runnable() {
            @Override
            public void run() {
                core.tapKey(HidUsages.K_ESC);
            }
        }));
        row.addView(actionButton("Vol\n−", new Runnable() {
            @Override
            public void run() {
                core.sendConsumer(HidUsages.C_VOLUME_DOWN);
            }
        }));
        row.addView(actionButton("Vol\n+", new Runnable() {
            @Override
            public void run() {
                core.sendConsumer(HidUsages.C_VOLUME_UP);
            }
        }));

        modeBtn = actionButton(MODE_LABEL[mode], new Runnable() {
            @Override
            public void run() {
                setMode((mode + 1) % 3);
            }
        });
        modeBtn.setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, 10.5f);
        modeBtn.setTextColor(Ui.ACCENT);
        row.addView(modeBtn);

        return row;
    }

    private Button holdButton(final String label, final int mask) {
        Button b = Ui.button(this, label, Ui.PANEL_2, Ui.TEXT, 13);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f);
        lp.leftMargin = lp.rightMargin = Ui.dp(this, 2);
        b.setLayoutParams(lp);
        b.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View v, MotionEvent event) {
                switch (event.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        core.setMouseButtons(core.mouseButtons() | mask);
                        v.setBackground(Ui.rounded(PadActivity.this, Ui.ACCENT, 10, Ui.ACCENT, 1));
                        return true;
                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_CANCEL:
                        core.setMouseButtons(core.mouseButtons() & ~mask);
                        v.setBackground(Ui.rounded(PadActivity.this, Ui.PANEL_2, 10, Ui.STROKE, 1));
                        return true;
                    default:
                        return false;
                }
            }
        });
        return b;
    }

    private Button actionButton(String label, final Runnable action) {
        Button b = Ui.button(this, label, Ui.PANEL_2, Ui.TEXT, 11);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1.35f);
        lp.leftMargin = lp.rightMargin = Ui.dp(this, 2);
        b.setLayoutParams(lp);
        b.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                action.run();
            }
        });
        return b;
    }

    /** 分屏 <-> 仅触控板 <-> 仅键盘；被隐藏的那一半不占高度，可视区域整块给另一半。 */
    private void setMode(int newMode) {
        mode = ((newMode % 3) + 3) % 3;
        applyMode();
    }

    private void applyMode() {
        LinearLayout.LayoutParams padLp = (LinearLayout.LayoutParams) pad.getLayoutParams();
        LinearLayout.LayoutParams keyLp = (LinearLayout.LayoutParams) keys.getLayoutParams();

        switch (mode) {
            case MODE_PAD:
                Ui.setVisible(pad, true);
                Ui.setVisible(keys, false);
                padLp.weight = 1f;
                keyLp.weight = 0f;
                break;
            case MODE_KEYS:
                Ui.setVisible(pad, false);
                Ui.setVisible(keys, true);
                padLp.weight = 0f;
                keyLp.weight = 1f;
                break;
            default:
                Ui.setVisible(pad, true);
                Ui.setVisible(keys, true);
                padLp.weight = 1.0f;
                keyLp.weight = 1.15f;
                break;
        }
        pad.setLayoutParams(padLp);
        keys.setLayoutParams(keyLp);

        if (modeBtn != null) {
            modeBtn.setText(MODE_LABEL[mode]);
            modeBtn.setTextColor(mode == MODE_SPLIT ? Ui.TEXT_DIM : Ui.ACCENT);
        }
        Prefs.setLayoutMode(this, mode);
    }

    /* ------------------------------ 配对面板 ------------------------------ */

    private View buildConnectPanel() {
        FrameLayout holder = new FrameLayout(this);
        holder.setBackgroundColor(Ui.BG);

        ScrollView sv = new ScrollView(this);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(Ui.dp(this, 18), Ui.dp(this, 26), Ui.dp(this, 18), Ui.dp(this, 24));
        sv.addView(col, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        holder.addView(sv, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        TextView title = Ui.text(this, "SwitchPad 键鼠", Ui.TEXT, 22);
        col.addView(title);
        TextView sub = Ui.text(this, "手机 = 蓝牙键盘 + 触控板，电脑侧无需安装任何程序", Ui.TEXT_DIM, 12.5f);
        sub.setPadding(0, Ui.dp(this, 4), 0, Ui.dp(this, 14));
        col.addView(sub);

        panelStatus = Ui.text(this, "正在初始化…", Ui.WARN, 13);
        panelStatus.setPadding(0, 0, 0, Ui.dp(this, 12));
        col.addView(panelStatus);

        LinearLayout stepBox = new LinearLayout(this);
        stepBox.setOrientation(LinearLayout.VERTICAL);
        stepBox.setBackground(Ui.rounded(this, Ui.PANEL, 12, Ui.STROKE, 1));
        stepBox.setPadding(Ui.dp(this, 14), Ui.dp(this, 12), Ui.dp(this, 14), Ui.dp(this, 12));
        panelHint = Ui.text(this,
                "⚠ 必须先让本页显示「已就绪」再配对：App 未注册时配上的对，"
                        + "电脑会缓存一份「没有 HID 服务」的记录，之后怎么连都是连一会儿就断。\n\n"
                        + "1. 打开电脑（Switch 上的 Windows）的蓝牙\n"
                        + "2. 点下面的「让电脑发现本机」\n"
                        + "3. 电脑：设置 → 蓝牙和其他设备 → 添加设备 → 蓝牙 → SwitchPad\n"
                        + "4. 配对成功后本面板自动消失，屏幕即键鼠\n"
                        + "5. 电脑侧发起没反应 → 用「从手机扫描电脑」反过来发起\n"
                        + "6. 已经配过却连不上 → 先在电脑上删除该设备（清缓存）再重配",
                Ui.TEXT, 12.5f);
        panelHint.setLineSpacing(Ui.dp(this, 4), 1f);
        stepBox.addView(panelHint);
        LinearLayout.LayoutParams boxLp = Ui.matchWrap();
        boxLp.bottomMargin = Ui.dp(this, 12);
        col.addView(stepBox, boxLp);

        LinearLayout btnRow = new LinearLayout(this);
        btnRow.setOrientation(LinearLayout.HORIZONTAL);

        Button btBtn = Ui.button(this, "打开蓝牙", Ui.PANEL_2, Ui.TEXT, 13);
        btBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                enableBluetooth();
            }
        });
        LinearLayout.LayoutParams p1 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        p1.rightMargin = Ui.dp(this, 5);
        btnRow.addView(btBtn, p1);

        discoverBtn = Ui.button(this, "让电脑发现本机", Ui.ACCENT, 0xFF06121E, 13);
        discoverBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                makeDiscoverable();
            }
        });
        LinearLayout.LayoutParams p2 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.6f);
        p2.leftMargin = Ui.dp(this, 5);
        btnRow.addView(discoverBtn, p2);
        col.addView(btnRow, Ui.matchWrap());

        /* --- 注册失败时的自救行 --- */
        LinearLayout fixRow = new LinearLayout(this);
        fixRow.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams fixLp = Ui.matchWrap();
        fixLp.topMargin = Ui.dp(this, 8);
        col.addView(fixRow, fixLp);

        Button retryBtn = Ui.button(this, "重试注册", Ui.PANEL_2, Ui.TEXT, 12.5f);
        retryBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (core.hasProxy()) {
                    core.start();
                } else {
                    startHudIfPossible();
                }
                refreshStatus();
            }
        });
        LinearLayout.LayoutParams f1 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        f1.rightMargin = Ui.dp(this, 5);
        fixRow.addView(retryBtn, f1);

        Button resetBtn = Ui.button(this, "重置 HID", Ui.PANEL_2, Ui.WARN, 12.5f);
        resetBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                core.resetHid();
                refreshStatus();
            }
        });
        LinearLayout.LayoutParams f2 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        f2.leftMargin = f2.rightMargin = Ui.dp(this, 5);
        fixRow.addView(resetBtn, f2);

        Button againBtn = Ui.button(this, "再次连接", Ui.ACCENT, 0xFF06121E, 12.5f);
        againBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                core.reconnectNow();
            }
        });
        LinearLayout.LayoutParams f0 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        f0.rightMargin = Ui.dp(this, 5);
        fixRow.addView(againBtn, f0);

        Button btSetBtn = Ui.button(this, "蓝牙设置", Ui.PANEL_2, Ui.TEXT, 12.5f);
        btSetBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                openBluetoothSettings();
            }
        });
        LinearLayout.LayoutParams f3 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        f3.leftMargin = Ui.dp(this, 5);
        fixRow.addView(btSetBtn, f3);

        /* --- 反向配对：手机主动扫描并绑定电脑 --- */
        LinearLayout scanHead = new LinearLayout(this);
        scanHead.setOrientation(LinearLayout.HORIZONTAL);
        scanHead.setGravity(Gravity.CENTER_VERTICAL);
        Button scanBtn = Ui.button(this, "从手机扫描电脑", Ui.PANEL_2, Ui.TEXT, 12.5f);
        scanBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                scanRows.clear();
                if (scanList != null) scanList.removeAllViews();
                boolean ok = core.startDiscovery();
                if (scanStatus != null) {
                    scanStatus.setText(ok ? "扫描中…（电脑需勾选「允许蓝牙设备查找此电脑」）" : "无法开始扫描");
                }
            }
        });
        LinearLayout.LayoutParams sh = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        sh.rightMargin = Ui.dp(this, 8);
        scanHead.addView(scanBtn, sh);
        scanStatus = Ui.text(this, "", Ui.TEXT_DIM, 11f);
        scanHead.addView(scanStatus, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.2f));
        LinearLayout.LayoutParams scanHeadLp = Ui.matchWrap();
        scanHeadLp.topMargin = Ui.dp(this, 8);
        col.addView(scanHead, scanHeadLp);

        scanList = new LinearLayout(this);
        scanList.setOrientation(LinearLayout.VERTICAL);
        col.addView(scanList, Ui.matchWrap());

        TextView scanTip = Ui.text(this,
                "已配对的电脑不用扫描，直接点下面的「连接 <电脑名>」即可；"
                        + "扫描是给「还没配对」的情况用的。",
                Ui.alpha(Ui.TEXT_DIM, 0.9f), 11f);
        scanTip.setPadding(0, Ui.dp(this, 4), 0, 0);
        col.addView(scanTip, Ui.matchWrap());

        diagText = Ui.text(this, "", Ui.alpha(Ui.TEXT_DIM, 0.95f), 11f);
        diagText.setLineSpacing(Ui.dp(this, 3), 1f);
        diagText.setBackground(Ui.rounded(this, 0xFF141A23, 10, Ui.STROKE, 1));
        diagText.setPadding(Ui.dp(this, 12), Ui.dp(this, 9), Ui.dp(this, 12), Ui.dp(this, 9));
        LinearLayout.LayoutParams diagLp = Ui.matchWrap();
        diagLp.topMargin = Ui.dp(this, 8);
        col.addView(diagText, diagLp);

        TextView knownTitle = Ui.text(this, "已配对设备（直接重连）", Ui.TEXT_DIM, 12);
        knownTitle.setPadding(0, Ui.dp(this, 16), 0, Ui.dp(this, 6));
        col.addView(knownTitle);

        bondedList = new LinearLayout(this);
        bondedList.setOrientation(LinearLayout.VERTICAL);
        col.addView(bondedList, Ui.matchWrap());

        TextView foot = Ui.text(this,
                "要求：Android 9（API 28）及以上，且手机蓝牙支持 HID Device 角色。\n"
                        + "电脑侧看到的是标准蓝牙键盘 + 蓝牙鼠标，Windows 无需驱动。",
                Ui.alpha(Ui.TEXT_DIM, 0.9f), 11.5f);
        foot.setPadding(0, Ui.dp(this, 18), 0, 0);
        foot.setLineSpacing(Ui.dp(this, 3), 1f);
        col.addView(foot);

        connectPanel = holder;
        return holder;
    }

    private void rebuildBondedList() {
        if (bondedList == null) return;
        bondedList.removeAllViews();
        if (!hasBtPermission()) return;
        List<BluetoothDevice> devices = core.bondedDevices();
        if (devices.isEmpty()) {
            TextView t = Ui.text(this, "（暂无配对记录，先让电脑发现本机）", Ui.TEXT_DIM, 12);
            bondedList.addView(t);
            return;
        }
        for (final BluetoothDevice d : devices) {
            String name;
            try {
                name = d.getName() == null ? d.getAddress() : d.getName();
            } catch (SecurityException e) {
                name = d.getAddress();
            }

            LinearLayout line = new LinearLayout(this);
            line.setOrientation(LinearLayout.HORIZONTAL);

            Button conn = Ui.button(this, "连接 " + name, Ui.PANEL_2, Ui.TEXT, 12.5f);
            conn.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    core.connect(d);
                }
            });
            LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
            clp.rightMargin = Ui.dp(this, 5);
            line.addView(conn, clp);

            // 长按 = 取消配对（配对缓存坏了的时候需要"删了重配"）
            Button drop = Ui.button(this, "解除", Ui.PANEL_2, Ui.WARN, 12f);
            drop.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    boolean ok = core.removeBond(d);
                    toast(ok ? "已解除配对（若电脑侧仍显示已配对，也要在电脑上删除）"
                            : "系统不允许 App 解除配对，请到「蓝牙设置」里删除");
                    core.queryUuids(d);
                }
            });
            line.addView(drop, new LinearLayout.LayoutParams(Ui.dp(this, 62), ViewGroup.LayoutParams.WRAP_CONTENT));

            LinearLayout.LayoutParams lp = Ui.matchWrap();
            lp.bottomMargin = Ui.dp(this, 6);
            bondedList.addView(line, lp);
        }
    }

    /* ------------------------------ 蓝牙开关 / 可见性 ------------------------------ */

    private void enableBluetooth() {
        BluetoothAdapter adapter = core.adapter();
        if (adapter == null) {
            toast("本机没有蓝牙适配器");
            return;
        }
        try {
            startActivityForResult(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE), REQ_ENABLE_BT);
        } catch (SecurityException e) {
            toast("无法打开蓝牙：" + e.getMessage());
        }
    }

    private void makeDiscoverable() {
        BluetoothAdapter adapter = core.adapter();
        if (adapter == null) return;
        if (!adapter.isEnabled()) {
            enableBluetooth();
            return;
        }
        try {
            Intent i = new Intent(BluetoothAdapter.ACTION_REQUEST_DISCOVERABLE);
            i.putExtra(BluetoothAdapter.EXTRA_DISCOVERABLE_DURATION, 120);
            startActivityForResult(i, REQ_DISCOVERABLE);
        } catch (SecurityException e) {
            toast("无法进入可发现模式：" + e.getMessage());
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_ENABLE_BT) {
            if (resultCode == RESULT_OK) {
                startHudIfPossible();
                makeDiscoverable();
            }
        } else if (requestCode == REQ_DISCOVERABLE) {
            if (resultCode > 0) {
                panelStatus.setText("已在可发现状态（" + resultCode + " 秒）：现在到电脑上添加蓝牙设备");
                panelStatus.setTextColor(Ui.OK);
            } else {
                panelStatus.setText("未能进入可发现模式，请检查权限");
                panelStatus.setTextColor(Ui.ERR);
            }
        }
    }

    /* ------------------------------ 状态显示 ------------------------------ */

    private void refreshStatus() {
        boolean conn = core.isConnected();
        if (conn) {
            statusText.setText("● 已连接 " + core.hostName() + " · " + core.linkType());
            statusText.setTextColor(Ui.OK);
            Ui.setVisible(connectPanel, false);
            return;
        }

        statusText.setText(core.isRegistered() ? "○ 已就绪，等待电脑连接" : "○ " + core.stateMessage());
        statusText.setTextColor(Ui.TEXT_DIM);
        Ui.setVisible(connectPanel, true);
        panelStatus.setText(core.stateMessage());
        panelStatus.setTextColor(core.isRegistered() ? Ui.OK : Ui.WARN);

        // 未连接时把诊断（含事件日志）摊开，方便定位"连上又断开"
        boolean showDiag = !conn && core.hasProxy();
        Ui.setVisible(diagText, showDiag);
        if (showDiag) diagText.setText(core.diagnosis());

        pad.setBridge(this);
        keys.setBridge(this);
    }

    private void openBluetoothSettings() {
        try {
            startActivity(new Intent(android.provider.Settings.ACTION_BLUETOOTH_SETTINGS));
        } catch (Exception e) {
            toast("打不开蓝牙设置：" + e.getMessage());
        }
    }

    private void refreshLeds(int bits) {
        StringBuilder sb = new StringBuilder();
        if ((bits & 0x01) != 0) sb.append("NUM ");
        if ((bits & 0x02) != 0) sb.append("CAPS ");
        if ((bits & 0x04) != 0) sb.append("SCR ");
        ledText.setText(sb.toString().trim());
        ledText.setTextColor(sb.length() > 0 ? Ui.ACCENT : Ui.TEXT_DIM);
    }

    /* ------------------------------ HidCore.Listener ------------------------------ */

    @Override
    public void onHidState(boolean registered, String message) {
        refreshStatus();
        HidService.updateNotification(this, core.isConnected()
                ? "已连接：" + core.hostName()
                : message);
    }

    @Override
    public void onConnection(boolean connected, String hostName) {
        if (!connected) {
            // 断开时清掉锁存的修饰键与本地按键态，避免"看起来还按着"
            keys.resetLatches();
        }
        refreshStatus();
        rebuildBondedList();
        if (connected) {
            toast("已连接：" + hostName);
        }
    }

    @Override
    public void onLeds(int bits) {
        refreshLeds(bits);
    }

    /* ------------------------------ HidCore.ScanListener ------------------------------ */

    @Override
    public void onDeviceFound(BluetoothDevice device, short rssi) {
        if (device == null) return;
        String name;
        String key;
        try {
            name = device.getName() == null || device.getName().isEmpty()
                    ? device.getAddress() : device.getName();
            key = device.getAddress();
        } catch (SecurityException e) {
            return;
        }

        String label = name + "  " + rssi + "dB  · " + HidCore.bondStateName(bondStateOf(device));
        Button existing = scanRows.get(key);
        if (existing != null) {
            existing.setText(label);
            return;
        }

        final BluetoothDevice target = device;
        Button b = Ui.button(this, label, Ui.PANEL_2, Ui.TEXT, 12f);
        b.setGravity(Gravity.LEFT | Gravity.CENTER_VERTICAL);
        b.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                core.stopDiscovery();
                if (bondStateOf(target) == BluetoothDevice.BOND_NONE) {
                    if (!core.bond(target)) {
                        toast("发起配对失败，检查权限或改用电脑侧配对");
                    }
                } else {
                    core.connect(target);
                }
            }
        });
        scanRows.put(key, b);
        if (scanList != null) {
            LinearLayout.LayoutParams lp = Ui.matchWrap();
            lp.topMargin = Ui.dp(this, 5);
            scanList.addView(b, lp);
        }
    }

    @Override
    public void onDiscoveryState(boolean discovering) {
        if (scanStatus == null) return;
        if (discovering) {
            scanStatus.setText("扫描中…");
        } else if (scanRows.isEmpty()) {
            scanStatus.setText("没扫到：电脑需勾选「允许蓝牙设备查找此电脑」，且最好用有线/外接蓝牙时它才广播");
        } else {
            scanStatus.setText("扫描结束，点上面的条目配对/连接");
        }
    }

    @Override
    public void onBondChanged(BluetoothDevice device, int bondState) {
        if (device == null) return;
        String key;
        String name;
        try {
            key = device.getAddress();
            name = device.getName() == null || device.getName().isEmpty() ? key : device.getName();
        } catch (SecurityException e) {
            return;
        }
        Button b = scanRows.get(key);
        String label = name + "  · " + HidCore.bondStateName(bondState);
        if (b != null) {
            b.setText(label);
            b.setTextColor(bondState == BluetoothDevice.BOND_BONDED ? Ui.OK : Ui.TEXT);
        }
        if (bondState == BluetoothDevice.BOND_BONDING) {
            toast("配对中，手机上若弹出确认请点允许");
        } else if (bondState == BluetoothDevice.BOND_BONDED) {
            toast("已配对：" + name);
            rebuildBondedList();
        }
    }

    private int bondStateOf(BluetoothDevice d) {
        try {
            return d.getBondState();
        } catch (SecurityException e) {
            return BluetoothDevice.BOND_NONE;
        }
    }

    /* ------------------------------ 触控板桥接 ------------------------------ */

    @Override
    public void moveBy(float dx, float dy, float dtMs) {
        core.moveBy(dx, dy, dtMs);
    }

    @Override
    public void wheelBy(float dyPx) {
        core.wheelBy(dyPx);
    }

    @Override
    public void setMouseButtons(int mask) {
        core.setMouseButtons(mask);
    }

    @Override
    public void tapMouseButton(int mask) {
        core.tapMouseButton(mask);
    }

    /* ------------------------------ 键盘桥接 ------------------------------ */

    @Override
    public void pressKey(int usage) {
        core.pressKey(usage);
    }

    @Override
    public void releaseKey(int usage) {
        core.releaseKey(usage);
    }

    @Override
    public void setModifier(int modIndex, boolean down) {
        core.setModifier(modIndex, down);
    }

    @Override
    public void releaseAllModifiers() {
        core.releaseAllModifiers();
    }

    @Override
    public void sendConsumer(int usage) {
        core.sendConsumer(usage);
    }

    @Override
    public void sendChord(int modifierBits, int[] usages) {
        core.sendChord(modifierBits, usages);
    }

    /* ------------------------------ 设置对话框 ------------------------------ */

    private void showSettings() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(Ui.dp(this, 18), Ui.dp(this, 8), Ui.dp(this, 18), Ui.dp(this, 8));

        // 指针速度：0.10x ~ 3.00x，步进 0.05x；一次整屏滑动 ≈ 540 * 速度 个 counts
        addSeek(box, "指针速度", 2, 60, Math.round(Prefs.sensitivity(this) * 20f), new ValueSetter() {
            @Override
            public void set(int progress, TextView title) {
                float v = progress / 20f;
                Prefs.setSensitivity(PadActivity.this, v);
                title.setText(String.format("指针速度 %.2fx", v));
                core.applySettings();
            }
        });
        // 滚轮：每个棘轮对应的手指位移（dp），越小越快
        addSeek(box, "滚轮灵敏度（数值越小越快）", 24, 96,
                Math.round(Prefs.scrollNotchDp(this)), new ValueSetter() {
            @Override
            public void set(int progress, TextView title) {
                Prefs.setScrollNotchDp(PadActivity.this, progress);
                title.setText("滚轮 每 " + progress + "dp 一格");
                core.applySettings();
            }
        });
        addCheck(box, "指针加速", Prefs.acceleration(this), new BoolSetter() {
            @Override
            public void set(boolean v) {
                Prefs.setAcceleration(PadActivity.this, v);
            }
        });
        addCheck(box, "自然滚动（手指上滑 = 内容上移）", Prefs.naturalScroll(this), new BoolSetter() {
            @Override
            public void set(boolean v) {
                Prefs.setNaturalScroll(PadActivity.this, v);
            }
        });
        addCheck(box, "轻点即左键", Prefs.tapToClick(this), new BoolSetter() {
            @Override
            public void set(boolean v) {
                Prefs.setTapToClick(PadActivity.this, v);
            }
        });
        addCheck(box, "长按后拖动 = 拖拽", Prefs.longPressDrag(this), new BoolSetter() {
            @Override
            public void set(boolean v) {
                Prefs.setLongPressDrag(PadActivity.this, v);
            }
        });
        addCheck(box, "兼容模式：SDP 只声明键盘（描述符 63B）", Prefs.compactDescriptor(this), new BoolSetter() {
            @Override
            public void set(boolean v) {
                Prefs.setCompactDescriptor(PadActivity.this, v);
                // 描述符是注册时写进 SDP 的，改完必须重新注册
                core.resetHid();
            }
        });
        addCheck(box, "触觉反馈", Prefs.haptics(this), new BoolSetter() {
            @Override
            public void set(boolean v) {
                Prefs.setHaptics(PadActivity.this, v);
                keys.setHaptics(v);
                pad.setHaptics(v);
            }
        });

        ScrollView sv = new ScrollView(this);
        sv.addView(box);

        new AlertDialog.Builder(this)
                .setTitle("设置")
                .setView(sv)
                .setPositiveButton("关闭", null)
                .setNegativeButton("退出应用", new android.content.DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(android.content.DialogInterface dialog, int which) {
                        core.stop();
                        stopService(new Intent(PadActivity.this, HidService.class));
                        finish();
                    }
                })
                .show();
    }

    private interface BoolSetter {
        void set(boolean v);
    }

    private interface ValueSetter {
        void set(int progress, TextView title);
    }

    private SeekBar addSeek(LinearLayout parent, String label, int min, int max, int init,
                            final ValueSetter setter) {
        final TextView t = Ui.text(this, label, Ui.TEXT, 12.5f);
        t.setPadding(0, Ui.dp(this, 8), 0, 0);
        parent.addView(t);
        final SeekBar sb = new SeekBar(this);
        sb.setMax(Math.max(1, max - min));
        final int clamped = Math.max(min, Math.min(max, init));
        sb.setProgress(clamped - min);
        parent.addView(sb, Ui.matchWrap());
        final int base = min;
        setter.set(clamped, t);   // 先把当前数值写进标题
        sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                setter.set(base + progress, t);
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
            }
        });
        return sb;
    }

    private void addCheck(LinearLayout parent, String label, boolean checked, final BoolSetter setter) {
        CheckBox cb = new CheckBox(this);
        cb.setText(label);
        cb.setTextColor(Ui.TEXT);
        cb.setTextSize(12.5f);
        cb.setChecked(checked);
        cb.setPadding(0, Ui.dp(this, 6), 0, Ui.dp(this, 2));
        cb.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton buttonView, boolean isChecked) {
                setter.set(isChecked);
                core.applySettings();
            }
        });
        parent.addView(cb, Ui.matchWrap());
    }

    private void toast(String msg) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show();
    }

    @Override
    public void onBackPressed() {
        if (mode != MODE_SPLIT) {
            setMode(MODE_SPLIT);
        } else {
            moveTaskToBack(true);
        }
    }
}
