# SwitchWOA 基准快照 2026-11-18（手柄可用 + WU 已拆 + sshd 自愈）

> 这一份是"确认可用"的定格状态：**Joy-Con 导轨手柄驱动 0.3.20 正常工作**、Windows Update 已彻底停用、
> sshd 有自动重启守护。后续要重构 **WiFi 驱动**，从这里出发；出问题就按本文的"还原"一节操作。

## 一、这个基准包含什么

| 路径 | 内容 |
|---|---|
| `switchjoycon\` | 手柄驱动工程全量（`src\`、`tools\`、`ref\`、`out\` 各版本产物、`HANDOFF.md`、`.clang-format`） |
| `device\baseline-20261118\` | **机器上同名目录的原样拷贝**（见下方说明） |
| `device\...\drivers\current-0320\` | 当前绑定的驱动包 **0.3.20**（sys 38824 B） |
| `device\...\drivers\fallback-029\` | 兜底包 **0.2.9 二进制**（sys 34216 B，sha256 `7EE996F8…`，以 0.3.12 版本号封装） |
| `device\...\tools\` | `sshd-watchdog.{ps1,cmd,vbs}`、`make-baseline2.ps1` |
| `device\...\state\` | `state.txt`（完整状态报告）、两个导轨设备的 `Device Parameters` 注册表导出、`switchjoycon` 服务键、`hosts.txt` |
| `device\...\RESTORE.ps1` / `restore.cmd` | **一键还原**（重装驱动 + 重挂守护任务 + 关 WU），幂等 |
| `device\...\MANIFEST.txt` | 每个文件的 SHA256 前 16 位 + 大小 |
| `host-tools\` | 这轮用到的全部 PC 侧脚本（离线修卡、清 WU、清 torn state、重建配置、sshd 守护、诊断）|
| `reports\` | 现场诊断日志（卡报告、chkdsk、WU 清理、配置重建、驱动部署报告）|

**为什么有两份**：`device\` 是机器上 `C:\SwitchWOA\baseline-20261118\` 的拷贝（可以在机器上直接双击
`restore.cmd`）；`switchjoycon\` 是 PC 上的工程源码与构建产物。两者都要保留。

## 二、定格时机器上的状态（详见 `device\...\state\state.txt`）

| 项目 | 值 |
|---|---|
| 系统 | Windows 11 专业版 ARM64 build 22631 |
| 手柄驱动 | **0.3.20.0**（驱动库 `oem50.inf`），两个设备 `OK`，`JcVer=320`，`JcState=9` |
| 摇杆 | 32 帧静止平均定中心（静止门槛 spread≤60 + 合理区间 2048±600 + 跨重连粘住 + 运行时自检），死区 200 读数 |
| 指针 | 左摇杆当鼠标，0.2.9 原算法（速度×毫秒），静止不发位移；`JcPointer=1` |
| WU | `wuauserv/UsoSvc/WaaSMedicSvc/DoSvc` 全 `DISABLED`；21 个 WU 计划任务全 `Disabled`；13 个更新端点在 `hosts` 里拉黑；`SoftwareDistribution` 已删除 |
| sshd | 服务 `Automatic`；守护任务 `DshSshdWatchdog`（每 2 分钟）+ `DshSshdWatchdogBoot`（开机 30 s）|
| 其它驱动 | `switchwifi` / `switchbt` / `switchtouch` / `switchfan` / `switchaudio` 均 Running |
| 桌面助手 | `restart-ssh.cmd`（检查/重启 sshd）、`reset-joycal.cmd`（清除 Windows 校准向导写的校准）|

## 三、还原（机器出问题回到这个状态）

机器上（管理员 PowerShell 或直接双击）：

```
C:\SwitchWOA\baseline-20261118\restore.cmd
```

它做四件事，全部幂等：
1. `pnputil /add-driver … \current-0320\switchjoycon.inf /install`，并把两个设备的 `JcPointer` 设回 1；
2. 重建 `DshSshdWatchdog` / `DshSshdWatchdogBoot` 两个隐藏守护任务；
3. 关掉四个 WU 服务、停用全部 WU 任务；
4. 打印两个手柄的最终版本与 `calValid`。

> 若要回到 **0.2.9 行为**（原始手感，有小跳动/误触）：用 `fallback-029` 那个 INF 安装即可
> （版本号 0.3.12 高于 0.2.9，`pnputil` 才肯"升级"；再要切回 0.3.20 用 `current-0320`，直接升）。

## 四、验证清单（每次改完驱动都跑一遍）

```powershell
# 1) 版本与状态
foreach ($id in 'ACPI\SWOA0E01\1','ACPI\SWOA0E02\2') {
  $v = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverVersion).Data
  $p = Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Enum\$id\Device Parameters"
  "$id ver=$v JcVer=$($p.JcVer) state=$($p.JcState) calValid=$($p.JcCalValid) centre=$('{0:X8}' -f [uint32]$p.JcCentre) raw=$('{0:X8}' -f [uint32]$p.JcRaw) hid=$('{0:X8}' -f [uint32]$p.JcHid) mouseSent=$($p.JcMouseSent)"
}
# 2) 判定标准
#    state=9、calValid=1、hid=00000000（静止时轴输出必须为 0）
#    mouseSent 在"手不动"的 10 秒内不增长（否则指针通道有问题）
#    JcFrames 与 JcReports 同步增长（说明每帧都被正确解析，无坏帧）
```

## 五、下一步：按 GitHub 的驱动重构 WiFi

这轮的经验对 WiFi 驱动直接适用（同样的 PnP/UART/ACPI 环境），切入点：

| 资料 | 位置 |
|---|---|
| WiFi 驱动工程 | `D:\paopao\WoA\...`（`switchwifi`，另见 `WoA\WiFi移植分析-Erista.md`、`WoA\WiFi上机步骤.md`、`switchfix\WiFi状态与坑-20261115.md`）|
| 上游 GitHub 参考 | 待定（重构时把 clone 位置记在本文件第六节）|
| 编译/签名/部署脚本 | `switchjoycon\tools\build.ps1`（LLVM clang-cl + WDK 签名），`device\...\RESTORE.ps1` 的 pnputil 用法 |
| 远程调试 | `ssh -i D:\paopao\ssh\woa_ed25519 woa@192.168.1.74`（局域网）或 `woa@100.106.117.117`（Tailscale）；辅助脚本 `host-tools\swrun.ps1` |
| 驱动诊断模式 | 注册表 `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\<设备>\<实例>\Device Parameters`，每秒写一次（不要更频繁，会拖慢系统）|

**教训（都来自这轮血泪）**：
1. 换驱动版本只用 `pnputil /add-driver … /install`（独立进程跑）；**绝不要**在设备管理器里"卸载设备 + 扫描硬件改动"（会卡死 PnP，整机冻结）；
2. `pnputil` 只升不降：回退版本 = 把旧二进制用更高 `DriverVer` 重新签名打包；
3. 任何"持续修正"的逻辑（自适应中心、累加器、跳变过滤）都会变成 bug 源：**一次性、有前提的测量 + 无状态输出**；
4. 脏关机是最贵的错误：一次强断电 → NTFS 脏位 → 索引损坏 → 用户配置丢失 → PHASE1 蓝屏；
5. 驱动自己每秒写注册表诊断没问题，别每毫秒写（0.3.0 之前就这么干过，把系统拖死）。

## 六、WiFi 重构记录（后续填写）

- 上游仓库 / commit：
- 本地工程路径：
- 第一版构建产物与版本号：
- 上机验证结果：
