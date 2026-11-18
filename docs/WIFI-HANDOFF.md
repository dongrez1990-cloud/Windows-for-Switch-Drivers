# WiFi 驱动重构 — 交接（2026-11-18）

> 给**下一个会话**：接手 WiFi 驱动重构前，先读完这一页。
> 手柄驱动已收尾冻结（见 `D:\paopao\baseline-20261118-joycon\BASELINE.md`），现在开始的是**无线网卡**这条线。

## 一、结论（三十秒版）

1. **"Lite 从代码层解决了 WiFi 驱动问题"是真的，代码在两个地方**：
   - GitHub 分支 `hdh001-winarm-bounded-write` —— 本次已 clone 到 `D:\paopao\wifi-ref\Windows-for-Switch-Drivers-hdh001\`；
   - 本地工作树 `D:\paopao\WoA\Windows-for-Switch-Drivers\` —— **同一仓库的 git 树，版本比 GitHub 更靠前**。
2. **GitHub 分支的 SwitchWifi = 0.5.7.0，与本地编译树 `WoA\rebuild\src\Drivers\drivers\SwitchWifi\` 逐字节相同**
   （80 文件、0 差异，SHA256 全量比对）。即：**Lite 的修复早就在我们编译用的源码里**。
3. **本地工作树已走到 0.7.2.0，但一次都没提交**（HEAD 仍是 11-08 的 `e220195`，
   19 个已跟踪文件改动 + 3 个未跟踪）。这是当前**最大风险**。
4. 机器上现在跑 **switchwifi 0.7.0.0（oem13.inf）**，设备 `ACPI\SWOA0D01\0`
   ("Switch Lite BCM4356 Wireless Adapter")，链路 **866.7 Mbps，已连上**。
5. **用户点名要修的是**：WiFi 掉线后**必须禁用/启用网卡（或重启）才能重连**。
   这正是 Lite 0.5.7 在代码层修掉的缺陷，而我们本地 0.7.2 用的是一套**绕法**。
   → **见第七节，这是首要任务。**

## 二、三份东西在哪、什么关系

| 位置 | 是什么 | 版本 |
|---|---|---|
| `D:\paopao\wifi-ref\Windows-for-Switch-Drivers-hdh001\` | 本次从 GitHub 拉下来的参考（`git clone --depth 1 -b hdh001-winarm-bounded-write`），HEAD `2d82f24`（11-17） | INF 0.5.7.0 |
| `D:\paopao\WoA\Windows-for-Switch-Drivers\` | **完整 git 工作树**（同一 remote），HEAD `e220195`（11-08）+ 22 个未提交改动 | INF **0.7.2.0** |
| `D:\paopao\WoA\rebuild\src\Drivers\drivers\SwitchWifi\` | 编译实际使用的源码（= 0.5.7 分支内容） | 0.5.7.0 |

远程分支：`hdh001-winarm-bounded-write`（Lite，2d82f24）、`main`（85fd4a2，**不含** `drivers/SwitchWifi`）、`sc7-test`（45f6258，SwitchWifi 0.5.5）。

## 三、Lite 到底"从代码层"解决了什么（逐版本）

摘自 `drivers/SwitchWifi/README.md`（49 KB）与 `NintendoSwitchPkg\docs\hdh001-home\WIFI.md`（32 KB）。

| 版本 | 修的东西 |
|---|---|
| 0.5.0 | 绑定 ACPI 枚举设备（ARM64 WinPE 22621.2861），跑通隔离方法与板级中断注册 |
| **0.5.1** | **固件事件投递**：控制 worker 只把事件转给 escan handler，`SET_SSID` 的成功/失败/断连永远到不了 Windows 关联处理器 → 改为统一分发 |
| **0.5.2** | **TX 在 EAPOL 排空 / 固件写密钥 / 对端授权期间被挂起**的窗口内保住新 TX |
| **0.5.3** | **NetAdapterCx 上下文属性**错误（`0xC0200211`，station 建不起来）+ **WDI teardown 保持 PASSIVE_LEVEL** |
| **0.5.4** | **host 候选选择**逻辑 + 记录是哪个 filter 拒了候选（此前 10 次 join 全拒、零密钥安装）|
| **0.5.5** | **预填固件接收环** + 处理 **bridged / length-encapsulated EAPOL**（解决动态密钥交换超时 `0x00048005`，Lite 首次真正上网）|
| 0.5.6 | 退役（`0xc00000b5` STATUS_IO_TIMEOUT；2 秒超时后无条件判死）|
| **0.5.7** | **断连失败策略缺陷**（就是"必须重启网卡"的根因，见第七节）：发出 disconnect 后 2 秒定时器无条件把适配器判死，即使射频已断开。改为到点用 BSSID 查询确认；down 事件也确认当前固件状态，避免迟到的旧事件打掉新连接；新增 `leave=phase/status` 诊断 |

底层配套（同在 0.5.x）：B01 的 PCIe 根端口/UPHY 时钟（PLLP HSIO、CAR 0x6D0 分频、PLL CTL10 表）、
`pchip` 从 AI EROM 描述符发现 CR4/D11/Gen2 核、固件+NVRAM（A3 = `brcmfmac4356A3-pcie.*`，固件 7.35.252.108 / FWID 01-65333037）、
msgbuf v5 控制/事件环、passive scan、CLM。

## 四、本地未提交的工作（0.5.8 → 0.7.2）——**第一件事**

```powershell
cd D:\paopao\WoA\Windows-for-Switch-Drivers
git status --porcelain      # 22 行
git diff --stat             # 19 files changed, 978 insertions(+), 56 deletions(-)
```

重点改动：

| 文件 | 改动 | 说明 |
|---|---|---|
| `drivers/SwitchWifi/windows/join.c` | +189 | 关联/断连路径（**第七节要改的就是这里**）|
| `drivers/SwitchWifi/windows/report.c` / `.h` | +61 / +22 | 诊断报告 |
| `drivers/SwitchWifi/windows/scan.c` / `.h` | +23 / +1 | 扫描 |
| `drivers/SwitchWifi/windows/chip.c` | +12 | 芯片/固件加载 |
| `drivers/SwitchWifi/core/station_frame.c`、`tests/test_frame.c` | 小 | 帧处理 + 测试 |
| `drivers/SwitchWifi/package/switchwifi.inf` | 6 | 版本号走到 0.7.2.0 |
| `drivers/SwitchWifi/windows/resources.c`、`station_data.c/.h` | 小 | 资源/站点数据 |
| `drivers/SwitchAudio/src/switchaudio.cpp`、`SwitchPowerButton/src/switchpwrbtn.c` | +278 / +180 | 与本线无关的并行工作 |
| `drivers/Tegra210Sdhc/src/nvtsdhc.c` / `.h` | +173 / +17 | SD 卡 UHS |
| `tools/Build-Driver.ps1`、`release/Make-Release.ps1`、`tools/cert/*.cer` | 小 | 构建/发布 |

未跟踪：`drivers/Tegra210Sdhc/src/t210_uhs.h`、`kits/Switch-Wifi/`、`tools/Build-SwitchWifi.ps1`（**WiFi 专用构建脚本**）。

**已固化的备份**（仓库被误清理也能恢复）：

```
D:\paopao\wifi-ref\uncommitted-0.5.7-to-0.7.2.patch   （163 KB, git diff --binary）
D:\paopao\wifi-ref\uncommitted-files.txt
D:\paopao\wifi-ref\uncommitted-stat.txt
```

> 建议立刻 `git add -A; git commit -m "SwitchWifi 0.5.8-0.7.2 WIP (Erista)"`。

## 五、机器状态（2026-11-18）

```
设备   : ACPI\SWOA0D01\0   "Switch Lite BCM4356 Wireless Adapter"   OK
驱动   : 0.7.0.0   (oem13.inf)   ← 比本地源码 0.7.2.0 旧一档
服务   : switchwifi  Running / Manual
网络   : WLAN Up, 866.7 Mbps
其它   : switchbt / switchtouch / switchfan / switchaudio / switchjoycon(0.3.20) 均 Running
```

版本 → 产物对照（`D:\paopao\WoA\Windows-for-Switch-Drivers\build\`）：
`SwitchWifi`(0.5.8) → `Fix`…`FixE`(0.7.0) → `FixF`(0.7.1) → `SwitchWifi072`(0.7.2)；
另有 `WoA\wd057\build\SwitchWifi057`(0.5.7)、`bringup\staged\switchwifi070`、`bringup\wifi-stage\switchwifi072`；
机器上用过历史包：`switchfix\backup-20261113\SwitchWOA\{drv072, wifi\driver-dbg(0.7.0), wifi\driver-recover(0.6.0)}`。

## 六、构建与上机

```powershell
# 构建（WiFi 专用脚本，未跟踪但现成）
pwsh -File D:\paopao\WoA\Windows-for-Switch-Drivers\tools\Build-SwitchWifi.ps1
# 主机侧单元测试（不需上机）：drivers\SwitchWifi\tests\*.c
```

上机（**只用这条路**）：

```powershell
scp -i D:\paopao\ssh\woa_ed25519 <新包>\* woa@192.168.1.74:C:/SwitchWOA/drivers/switchwifi-XXX/
Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
  CommandLine = 'cmd.exe /c C:\Windows\System32\pnputil.exe /add-driver C:\SwitchWOA\drivers\switchwifi-XXX\switchwifi.inf /install > C:\SwitchWOA\wifi-install.log 2>&1'
}
Get-PnpDevice -InstanceId 'ACPI\SWOA0D01\0' | Select Status
(Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0D01\0' -KeyName DEVPKEY_Device_DriverVersion).Data
Get-NetAdapter | Select Name,Status,LinkSpeed
```

铁律（手柄那轮血泪，WiFi 同样适用）：

1. **绝不**在设备管理器里"卸载设备 + 扫描硬件改动"（= `remove-device` + `scan-devices`，卡死 PnP/服务线程，SSH 只 TCP 通、banner 超时）；
2. `pnputil` **只升不降**：回退版本要把旧二进制用更高 `DriverVer` 重新签名打包
   （现成脚本 `D:\paopao\switchjoycon\tools\pack-029-as-036.ps1`，改参数即可）；
3. 驱动诊断写注册表**每秒一次足够**（每毫秒写会把整机拖死）；
4. 强制关机代价极高（11-18 修了 3 小时：脏位→索引损坏→配置丢失→PHASE1 蓝屏）；
5. sshd 卡死用桌面 `restart-ssh.cmd`，或等计划任务 `DshSshdWatchdog`（每 2 分钟自检 banner）自愈。

## 七、★ 首要任务：移植 Lite 的断连策略（解决"必须重启网卡才能重连"）

### 7.1 症状与机理

- 表现：WiFi 掉线后连不上，**禁用/启用网卡或重启才恢复**；
- 机理：驱动在"发 disconnect 后 2 秒仍无终态固件事件"时直接置适配器失效
  （`SwifiPnpFailure()` → `WdfDeviceSetFailed(WdfDeviceFailedNoRestart)`，`windows/device.c:21`），
  一旦进入只有禁用/启用设备或重启才能复位；
- 关键：**"射频其实早就断了"** 是最常见情况，却和"固件真卡住"走了同一条失败路径。

### 7.2 Lite 0.5.7 的解法（`wifi-ref\Windows-for-Switch-Drivers-hdh001\drivers\SwitchWifi`）

| 位置 | 内容 |
|---|---|
| `windows/join.c:549-562` | `LinkIsDown()`：向固件发命令 `0/23`（等价 `WLC_GET_BSSID`）；`BCME_NOTASSOCIATED(-17)` 或返回 6 字节全零 → 确认已断开；仍持旧 BSSID 或长度不符 → 协议错误 |
| `windows/join.c:593-606` | `ConfirmLinkDown()`：到点先 `SwifiReportLeave(Readback)`，再 `LinkIsDown()`：**确认已断 → `Down()` 正常收尾（清密钥、报去关联、媒体断开、试下一个候选），网卡保持可用**；未确认/IO 错误 → `SwifiReportLeave(Readback, status)` + `Poison()` 才走失败路径 |
| `windows/join.c:611-636` | `BeginLeave()`：断连四阶段 —— `LeaveCommand`(1 发命令) → `LeaveEvent`(2 等事件) → `LeaveReadback`(3 回读) → `LeaveConfirmed`(4 已确认) |
| `windows/report.h:28-29` | `SWIFI_LEAVE_PHASE`：`SwifiLeaveNone/Command/Event/Readback/Confirmed` |
| `windows/report.c:40,164-169` | `SwifiReportLeave()`；报告串带 **`leave=phase/status`**，能区分"命令超时"与"丢事件" |

### 7.3 我们本地 0.7.2 的现状（`WoA\Windows-for-Switch-Drivers\drivers\SwitchWifi`）

- `ConfirmLinkDown()`、`SwifiLeave*` 枚举、`SwifiReportLeave()` **全部不存在（各 0 处）**；
- `windows/join.c:707-732` 的 `JoinTimeout()`：2 秒到点也调 `LinkIsDown()`，但**无论结果**都执行
  `Finish(Leave)` → `J->FirmwareDirty = TRUE` → `JoinReset(J)` → **`J->Poisoned = FALSE`** → 唤醒等待者。
  即**强行清掉中毒状态**、把固件标记为脏，靠"下一次连接自己重置"蒙混（`FirmwareDirty` 本地出现 7 次）；
- 后果：真卡住时会被反复带进坏状态；正常断开也失去了 Lite 那套"确认后干净收尾"。

### 7.4 移植步骤

1. 引入 `windows/report.h` 的 `SWIFI_LEAVE_PHASE` + `windows/report.c` 的 `SwifiReportLeave()` 与报告串 `leave=%u/%08X` 字段；
2. 引入 `ConfirmLinkDown()`，整体替换 `JoinTimeout()` 里 `if (J->Leaving)` 那段：
   - 确认已断 → `Down(J, reason)`（不入失败路径）；
   - 未确认 / IO 错误 → `SwifiReportLeave(Readback, status)` + `Poison(status)`；
3. 删掉 `FirmwareDirty` 绕法（或仅保留在"未确认但重发 deauth 成功"的窄分支）；
4. `BeginLeave()` 补 4 阶段上报（照抄 0.5.7 的 611-636 行）；
5. **验收**：`leave=` 如实反映阶段；连续"断开→重连"5 次，全程不禁用/启用网卡、不重启，
   `Get-PnpDevice -InstanceId 'ACPI\SWOA0D01\0'` 始终 `OK`、`Code 0`。

### 7.5 已备好的对比材料

```
D:\paopao\wifi-ref\diff-join-0.5.7-vs-local-0.7.2.patch        （60 KB）
D:\paopao\wifi-ref\diff-report-0.5.7-vs-local-0.7.2.patch      （9.5 KB）
D:\paopao\wifi-ref\diff-report-h-0.5.7-vs-local-0.7.2.patch    （7.1 KB）
D:\paopao\wifi-ref\Windows-for-Switch-Drivers-hdh001\          （0.5.7 参考源码，可直接 grep）
```

## 八、还没被验证的（官方 README 明说）

- 持续吞吐（sustained throughput）；
- 组密钥重协商（group rekey）；
- **反复断连/重连的稳定性**（0.5.7 只解决"一次断连被判死"，多次往返未测）；
- Erista（初代机）与 B01（Lite）在 PCIe 车道/UPHY 时钟/供电轨/PH0-PH1 GPIO 上的差异
  —— 我们这台是 **Erista**，见 `WoA\WiFi移植分析-Erista.md`，Lite 的 B01 参数**不能照抄**。

## 九、建议的下一步（按优先级）

1. **固化**：commit 那 22 个改动；确认 0.7.2 能构建出同 hash 的产物；
2. **第七节移植**（用户点名的核心）；每改一处先跑 `tests\*.c` 主机测试，再上机；
3. **对齐版本**：机器 0.7.0.0 vs 本地 0.7.2.0 —— 看清 0.7.1/0.7.2 改了什么再决定是否上机；
4. **复现 Lite 验收路径**：`leave=phase/status` → 扫描 → 关联 → DHCP → 断连/重连 5 次；
5. **Erista 专项**：对照 `WoA\WiFi移植分析-Erista.md` 核 PCIe 根端口/UPHY/供电/GPIO（`t210` 与 `t214` 分工）；
6. 可选：把 `WifiProbe.inf`（离线 PCIe/固件/扫描自检）搬到 Erista，先证明硬件路径再谈驱动。

## 十、文档索引

| 文档 | 内容 |
|---|---|
| `wifi-ref\Windows-for-Switch-Drivers-hdh001\drivers\SwitchWifi\README.md` | 驱动自身 README（49 KB）：版本演进、缺陷分析、参考的 WDI/brcmfmac 代码 |
| `WoA\rebuild\src\NintendoSwitchPkg\docs\hdh001-home\WIFI.md` | Lite 上机全记录（32 KB）：bring-up、固件加载、msgbuf、passive scan、物理验收 |
| `WoA\WiFi移植分析-Erista.md`（62 KB） | Erista 的 WiFi 移植分析（硬件差异、寄存器、固件） |
| `WoA\WiFi上机步骤.md` | 上机操作步骤 |
| `switchfix\WiFi状态与坑-20261115.md` | 11-15 实测状态与踩坑记录 |
| `baseline-20261118-joycon\BASELINE.md` | 手柄线收尾 + 通用工程纪律（构建/签名/部署/验证） |
