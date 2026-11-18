# SwitchKey — 手机当蓝牙键盘 + 触控板，给 Switch 上的 Windows 11 用

把安卓手机变成一台**复合蓝牙 HID 设备**：一个蓝牙连接同时提供键盘和触控板。
Windows 11 会把它当成真的硬件键盘和鼠标，所以输入法（微软拼音）、快捷键、终端、
聊天框、登录界面**全部照常工作**——电脑上不需要装任何东西。

> 目标场景：Switch 刷了 WoA（Windows 11 ARM），出门不想带键盘和鼠标。
> 蓝牙上只有一个设备，从根上避免"鼠标连上把键盘挤掉"。

产物：`SwitchKey.apk`（约 48.5 KB，**v1.6**，minSdk 28 / Android 9+，targetSdk 34）

两种传输可选，`⋯` 菜单里切换，同一时间只跑一种：

| 传输 | 说明 |
|---|---|
| **经典 HID** | `BluetoothHidDevice`，配对流程见下文第二节 |
| **BLE HOGP** | `BluetoothGattServer` + 0x1812 服务，走 LE 链路。**不需要「可被发现」**，一直在广播，直接在 Windows 添加设备里找 |

---

## 零、BLE HOGP 模式（v1.4 新增）

`⋯` → **传输：经典 HID → 切到 BLE（HOGP）**。

GATT 服务表（0x1812）：

| 特征 | 属性 | 值 |
|---|---|---|
| 0x2A4A HID Information | read | `11 01 00 02`（bcdHID 1.11 / 国家码 0 / NormallyConnectable） |
| 0x2A4B Report Map | read | 复合键盘+鼠标报告描述符（129 字节） |
| 0x2A4C HID Control Point | write no response | — |
| 0x2A4E Protocol Mode | read / write no response | 默认 `0x01`（Report 模式） |
| 0x2A4D Report（键盘输入，id=1） | read/write/notify | 8 字节，靠 **notify** 推 |
| 0x2A4D Report（键盘 LED 输出，id=1） | read/write | 1 字节 |
| 0x2A4D Report（鼠标输入，id=2） | read/write/notify | 4 字节 |
| 0x2A22 Boot Keyboard Input | read/notify | Boot 协议兜底 |
| 0x2A32 Boot Keyboard Output | read/write | Boot 协议兜底 |

> 规范里容易漏的一条：每个 0x2A4D Report 特征都必须挂
> **Report Reference 描述符 0x2908**（两字节：report id + report type），
> 否则主机无法把报告映射到具体 report id。本实现已加。

广播：`AdvertiseData` 带 16-bit 服务 UUID 0x1812，名字放 scan response，
`setConnectable(true)`，低延迟模式。

**如果连上了但打不出字**，先看诊断日志里有没有这一行：
`CCCD 0x2A4D(键盘) -> 通知已开启`。没有的话，用菜单里的
**BLE: CCCD=自动 → 改成手动补** 再试一次（不同 ROM 的 GATT server 对
CCCD 0x2902 是自动补还是要手动加，实现不一致，所以做成了运行时开关）。

---

---

## 一、界面说明（v1.2 起布局更紧凑）

顶部只有**一行**：左边是状态文字，右边是 `切到鼠标` 和 `⋯`。
其余高度全给键盘 / 触控板；底部一行小字显示当前状态说明。

点 `⋯`（或点状态文字）弹出的菜单：

| 菜单项 | 作用 |
|---|---|
| 启动键盘 / 重新注册 | 重新走一遍 HID 注册 |
| 连接已配对的电脑（推一次连接） | 列出手机已配对设备，**由手机主动发起** HID 连接。有些主机不会主动连 HID 设备，这条是关键补刀 |
| 重连上次的电脑 | 主动连回上次的主机 |
| 让手机可被发现（配对） | **配对入口**，会自动等注册完成再弹窗 |
| 显示/隐藏诊断日志 | 看蓝牙每一步到底发生了什么 |
| 开启/关闭仅键盘兼容模式 | 换描述符（之后需在 Windows 重新配对） |
| 指针速率：60Hz / 30Hz 省链路 / 不限速 | **v1.6 新增**。蓝牙 UART 还停在 115200 bps 时用 30Hz 明显更稳 |
| 复制诊断日志 | 把日志拷到剪贴板，方便发给我看 |

## 二、正确配对流程（顺序很重要）

> ⚠️ 顺序错了就会出现「Windows 里显示在**其他设备**、已配对但打不出字」——
> 因为 Windows 配到的是一个**没有 HID 服务**的空壳设备。

1. 手机打开蓝牙 → 打开 SwitchKey → 状态变**黄色「键盘已就绪」**。
   这一步代表 `registerApp()` 真的成功了，HID 服务已经挂上。
2. 点 `⋯` → **让手机可被发现（配对）**。App 会确认注册完成后才弹"可被发现"对话框。
3. Windows：设置 → 蓝牙和设备 → **添加设备** → 蓝牙 →
   列表里找 **你手机的蓝牙名**（不是 SwitchKey，SwitchKey 是服务名；菜单标题里会显示当前蓝牙名）。
4. 手机弹出配对确认 → 允许（会出现 6 位配对码，两边点确认）。
5. 连上后手机状态变**绿色「已连接 · 电脑名」**，Windows 里会出现
   `HID Keyboard Device`（和 `HID-compliant mouse`）。

**如果之前在「其他设备」里留了旧的配对记录，必须先删掉再重配**：
设置 → 蓝牙和设备 → 点那个设备 → 删除设备。

## 三、键盘模式

| 操作 | 效果 |
|---|---|
| 一根手指按住 Shift，另一根按字母 | 组合键（Shift+A） |
| 轻点一下 Shift（松手）再按字母 | 一次性 Shift，作用于下一个键 |
| 400ms 内连点两次 Shift | 锁定大写，再点一下解除 |
| Ctrl / Alt / Win 同理 | Ctrl+C、Alt+Tab、Win+E |

中文输入：手机只发**键码**，拼音由电脑端输入法完成（用微软拼音）。

## 四、鼠标模式（不用带实体鼠标）

点右上角 `切到鼠标`：

| 操作 | 效果 |
|---|---|
| 单指拖动 | 移动指针 |
| 轻点 | 左键单击 |
| 两指轻点 | 右键单击 |
| 两指拖动 | 滚轮上下 |
| 底部三个大键 | 左键 / 中键 / 右键（按住=拖动选中） |

指针帧率在 `⋯` → **指针速率** 里切：

| 档位 | 何时用 |
|---|---|
| 60Hz（默认） | 慢速拖选、点小图标 |
| 30Hz 省链路 | 蓝牙串口还是 115200 bps、指针发飘/掉帧时 |
| 不限速 | 已经打了 hcd 补丁跑 3Mbaud，或走 BLE 时最跟手 |

> 按键按下/抬起、滚轮永远是**立即**发报，不受帧率限制——点击手感不会变木。

## 五、打不出字怎么查（用诊断日志）

点 `⋯` → **显示诊断日志**，然后再去按一个键或者重连一次，看日志顺序：

| 日志 | 含义 |
|---|---|
| `registerApp: 键盘+鼠标描述符 129 bytes` | 开始注册 |
| `onAppStatusChanged registered=true` | 注册成功（状态变黄） |
| `连接状态 -> 2 (...)` | 主机连上（2 = CONNECTED） |
| `主机在问 HID 报告 —— 说明 Windows 已经把 HID 驱动挂上了` | **最关键的绿灯**：说明 Windows 真的认了这个 HID 设备 |
| `sendReport 返回 false` | 通道没建立好，主机没在收 |

如果一直没等到"主机在问 HID 报告"，说明 Windows 挂了驱动失败——
把设备删掉重配；还是不行就打开**仅键盘兼容模式**（换单 collection、不带 report id
的最保守描述符），再删设备重配。

## 五之二、Windows 端配对失败、输入设备全卡死

症状：点「添加设备」里的手机 → 提示失败，然后**连蓝牙鼠标都点不动了**。
原因：Switch 这颗组合无线芯片在 WoA 驱动下扛不住第二条 HID 链路，蓝牙栈被顶崩。

恢复：

1. 用 Switch 的**触摸屏**点掉窗口（Win11 支持触摸，不依赖鼠标）。
2. `Win+A` → 关掉蓝牙再打开。
3. 还不行：管理员 PowerShell `Restart-Service bthserv -Force`。
4. 最坏重启一次。

正确姿势：**配对时只留手机一个蓝牙设备**——先把蓝牙鼠标断开/关掉，
配对成功并确认能打字后，再考虑要不要加回鼠标（加了大概率又会抢链路）。

## 六、掉线 / 和实体鼠标抢链路

Switch 里那颗 Broadcom 组合无线芯片在 WoA 驱动下多链路余量很小，
后连上的设备会把先前那条挤掉（掉线是**主机侧**先断的）。

1. **只用本 App**：实体鼠标断开，用触控板 → 蓝牙上只剩一条链路。
2. 实体鼠标有 2.4G USB 接收器就插扩展坞走 2.4G。
3. 本 App 已内置：掉线后自动退避重连（1s→2s→…→30s）、20 秒链路保活、
   `⋯` 里的「重连上次的电脑」。
4. Windows 侧：设备管理器 → 蓝牙适配器 → 电源管理 → 取消「允许计算机关闭此设备以节省电源」。

## 七、不支持 HID 的手机

状态变红字「本机不支持蓝牙 HID 设备模式」即为此情况（不少国产 ROM 阉割）。
备选：① 蓝牙转 USB HID 转换器插扩展坞；② Win11 跑 TCP + SendInput 小服务，
手机通过 Wi-Fi 发文本（能直接发中文）。

## 八、代码结构

```
SwitchKey/
├─ SwitchKey.apk                  构建产物（v1.2）
├─ build.ps1                      不用 Gradle 的打包脚本
├─ debug.keystore                 调试签名（脚本自动生成）
├─ tools/
│  ├─ zipadd.mjs                  把 classes.dex 塞进 aapt2 产物
│  ├─ gen-icon.mjs                程序化生成应用图标
│  ├─ run-tests.ps1               一键跑三个单元测试
│  ├─ KeyStateTest.java           按键状态机测试（20 项）
│  ├─ HidDescriptorTest.java      HID 描述符解析测试（10 项）
│  └─ PointerPumpTest.java        指针合并/限速测试（13 项，假时钟）
└─ app/src/main/
   ├─ AndroidManifest.xml
   ├─ res/values/strings.xml, res/mipmap/ic_launcher.png（生成）
   └─ java/com/paopao/switchkey/
      ├─ HidDescriptors.java      复合描述符 + 仅键盘描述符（纯 Java，可测）
      ├─ HidKeyboard.java         注册、发送报告、自动重连、保活、诊断日志
      ├─ HidService.java          前台服务保活
      ├─ MainActivity.java        一行状态条 + ⋯ 菜单 + 诊断面板 + 模式切换
      ├─ KeyboardView.java        Canvas 多指键盘
      ├─ KeyboardLayout.java      键盘布局（每行宽度合计 12 单位）
      ├─ MousePadView.java        触控板
      ├─ PointerPump.java         指针报告合并/限速（纯 Java，可测）
      └─ KeyState.java            修饰键状态机（纯 Java，可测）
```

## 九、重新构建 / 跑测试

```powershell
powershell -ExecutionPolicy Bypass -File D:\paopao\SwitchKey\build.ps1
powershell -ExecutionPolicy Bypass -File D:\paopao\SwitchKey\tools\run-tests.ps1
```

工具链在 `D:\paopao\.androidkit`（JDK 17 + Android cmdline-tools + build-tools 34）。

## 十、实现要点（给以后的自己）

- 复合描述符：`Report ID 1` = 键盘（8 字节负载），`Report ID 2` = 鼠标（4 字节负载）。
  用了 report id 之后**所有**报告都必须带 id；`sendReport(device, id, data)` 的
  data **不含** id（官方文档："Report data, not including Report Id"），id 由蓝牙栈加在报文头。
- **配对前必须确认 `onAppStatusChanged(true)`**。Windows 扫到的设备如果 SDP 里没有 HID 服务，
  配对后会落在「其他设备」里、永远打不出字，且必须删设备重配才能恢复。
- `BluetoothHidDevice.registerApp()` 在 API 33 改成带 `Executor` 的重载，
  旧重载在新 `android.jar` 里取不到 → 反射两个签名都试。
- `BluetoothAdapter.setScanMode()` 是隐藏 API，只能用 `ACTION_REQUEST_DISCOVERABLE`。
- 注册成功即启动前台服务，避免去电脑配对的路上被系统回收。

## 十一、和 Switch 蓝牙驱动的关系（v1.6 的设计依据）

对照两处：上游 `Windows-for-Switch-Drivers/drivers/SwitchBluetooth/src/switchbt.c`
（就是 GitHub 上那个蓝牙驱动的源码）与本地装好的 `D:\paopao\WoA\drivers\Switch-Bluetooth\`
（`switchbt.inf` + `README-ja.md`）。

**事实链**

| # | 事实 | 出处 |
|---|---|---|
| 1 | 蓝牙核是 BCM4356A3（Wi-Fi/蓝牙组合片），挂在 **UART-D（0x70006300）**、中断 GSI 122 | `switchbt.c` 头注释 |
| 2 | ACPI 设备 `BTH0` → 硬件 ID `ACPI\SWOA0B01` | `switchbt.inf` |
| 3 | 驱动只做 H4 传输层：建子设备 `SWOA\BTH_UART_H4`（兼容 ID `MS_BTHX_BTHMINI`）交给 `bth.inf` | `switchbt.c` 1.0.0.6 段 |
| 4 | 因此上面跑的是 **Windows 原版蓝牙栈**（BthMini + bthport + kbdhid/mouhid） | 同上 |
| 5 | 出厂 ROM 固件只有 **115200 bps**；打 Broadcom `BCM4356A3.hcd` 补丁 + FC18 才上 3 Mbaud | `README-ja.md` 第 3 段 |
| 6 | 驱动注释原话：`a keyboard works; audio broke up every ~0.7 s` —— 键盘这种小报文走得通，A2DP 才吃力 | `switchbt.c` 1.0.0.10 |
| 7 | 天线周边没在跑（WiFi 侧），必须离得很近 | `README-ja.md` 开头 |
| 8 | T214（续航版/OLED）在 1.0.0.18、1.0.0.19 里有单独处理 | `switchbt.c` 1.0.0.18/19 |

**对应到本 App 的取舍**

- **Windows 端零安装**：图里 `SWOA\BTH_UART_H4 → BthMini → bthport` 这一段已经由驱动解决，
  手机只要当一个标准 HID 外设（SDP 里挂 HID 记录）就够，不需要往 Switch 里塞任何输入服务。
- **不碰协议栈内部**：只用 `BluetoothHidDevice` 注册 HID SDP、发 report，不模拟厂商私有命令，
  这样驱动换版本（1.0.0.13 → 1.0.0.19）都不用跟着改。
- **帧率就是带宽**（事实 5）：115200 bps 时一串 ACL 报文很贵，所以 v1.6 把触控板位移合并成
  16ms 一帧，并给 `⋯` 菜单加了 30Hz 省链路档；等你在 Switch 上打完 hcd 补丁跑 3 Mbaud，
  再切「不限速」最跟手。
- **只留一条链路**（事实 1 + 总览表「Bluetooth」行）：WiFi 和蓝牙共用 BCM4356A3，
  多链路余量小，配好本 App 就先把实体蓝牙鼠标断开。
- **三种机型同一套用法**：总览表里初代机（T210）那栏是「适配复位、电源及固件版本」，
  续航版/OLED 是「复用主体；核对启动与唤醒」——差异全在驱动侧，手机端不用区分。

**睡眠 / 休眠之后连不上**

Win11 的「关机」是快速启动（≈休眠），驱动不一定重载。顺序：

1. 本 App `⋯` → **重连上次的电脑**，或者让 Windows 重新点一次设备；
2. 还不行就在 Switch 上跑 `Bluetooth.ps1 -Mode Retry`（1.0.0.13 起不用重启）；
3. 最后才考虑删设备重配。

**想让指针更跟手（可选）**

在 Switch 上把 Broadcom 的 `BCM4356A3.hcd` 交给驱动，跑一次：

```bat
powershell -ExecutionPolicy Bypass -File C:\SwitchWOA\Bluetooth\Bluetooth.ps1 -Mode Firmware -Path <BCM4356A3.hcd 的路径>
```

再重启，`-Mode Status` 里 `stage 3` 那行能看到 UART 时钟/速率/固件结果。上了 3 Mbaud 之后
本 App 切「不限速」才是真流畅。

