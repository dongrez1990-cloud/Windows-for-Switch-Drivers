# 初代机（Erista / T210）Windows 驱动栈 —— 从零构建到当前状态

> 这份文档写给**拿到初代 Nintendo Switch（Erista，T210）**、想复刻同一套 Windows 环境的人。
> 全部源码、已签名成品、构建脚本、UEFI 补丁和引导配置都在本仓库的三条分支里，
> 既能**从零编译**，也能**直接装成品**。
>
> 作者机器实测状态（2026-11-18）：Windows 11 ARM64，Wi-Fi `Up 866.7 Mbps`，
> 连续 5 次断连重连 + 10 轮浸泡全部自动恢复，全程无需禁用/启用网卡。

---

## 零、先确认你的机器是哪一台

| 机型 | 代号 | 是否适用本文 |
|---|---|---|
| 初代 Switch / OLED（Tegra X1 / T210） | **Erista** | ✅ 本文 |
| Switch Lite（Tegra X1+ / T214） | HDH-001 / Mariko | ❌ 请直接看上游 [`ruje0504-2/Windows-for-Switch-Drivers`](https://github.com/ruje0504-2/Windows-for-Switch-Drivers) 的 `docs/hdh001/` 文档 |

原因：上游主线是**为 Lite 写的**（`SWITCH_LITE_TOUCH`、`SWITCH_LITE_GPU`、T214 的
DFLL/EMC/ACTMON 时钟树）。本文这三条分支是**把同一套东西适配到 Erista** 的产物。

## ★ 你需要自己做什么（先看这张表）

| 类别 | 内容 | 你要做什么 |
|---|---|---|
| **A. 直接装，不用构建**<br>（13 套） | switchwifi 0.7.4 / 0.7.2 / 回退包、switchbt 1.0.0.19、switchfan 1.0.0.22、switchaudio 1.0.0.17、switchbattery 1.0.0.11、switchpwrbtn 1.0.0.4、switchsio 0.2.3.0、tegra210i2c 1.0.0.5、switchjoycon 0.3.20、tegra210sdhc 0.4.0.1 | 只需导入证书 → `pnputil` 安装（第五节）。**签名私钥不是你的，但你装现成包不需要私钥**，只需要信任它们的证书 |
| **B. 有成品，但很可能要你自己重签**<br>（4 套） | switchtouch 1.0.0.4、switchrtc 1.0.0.2、switchsensor 1.0.0.1、tegra210gpio 1.0.0.4 | 这 4 个由**第三张证书**（`A9E1F054…`）签名，而**作者手里也没有这张证书的私钥**（只剩指纹记录）。<br>→ 若你的机器不信任它：用 `erista-0.7.x` 源码重编这 4 个，再用你的证书签名（§4.2）。<br>→ 或者从任何一台已信任该证书的机器导出它的 `.cer` 并导入 |
| **C. 固件已给成品，不用编译** | `firmware\winx-v39\{bl31.bin, bl33.bin}`（UEFI/EDK2 成品）、`firmware\hekate\hekate_ctcaer_6.5.5.bin`、`esp\startup.nsh`、`esp\bootloader\hekate_ipl.ini` + 41 个启动项 | 直接拷到 SD 卡用。**只有你想改 UEFI 代码时**才需要按 §4.3 重新编译 `bl33.bin` |
| **D. 必须自己准备（不是构建，是获取）** | Windows 11 **ARM64** 镜像、SD 卡、hekate 启动方式（RCM 注入或 Atmosphere） | 这些不在仓库里，也不该在 |

> **一句话总结**：拿到这套东西，Erista 机器上**只有 4 个驱动可能需要你自己重编重签**（原因是一张证书的私钥丢失），其余全部可以直接装。

## 一、三条分支分别是干什么的

仓库：`https://github.com/dongrez1990-cloud/Windows-for-Switch-Drivers`

| 分支 | 内容 | 用途 |
|---|---|---|
| **`erista-0.7.x`**（+ tag `erista-0.7.4.0`） | 板级驱动源码：**Wi-Fi 0.7.4**、电源键、音频、SD 卡(UHS)、14 个主机测试、构建脚本 | 编驱动 |
| **`uefi/erista`** | UEFI 引导层（WinX/EDK2）改动：Wi-Fi+蓝牙初始化、`t210/` 阶段表、ACPI/电源表、SD、`WifiHandoff`（UEFI→Windows 交棒） | 编 `bl33.bin` |
| **`restore/20261118`**（本文所在分支） | **17 套已签名成品包**（版本与作者机器实测一致）、**UEFI 成品固件**（`bl31/bl33`）+ hekate 负载、手柄驱动源码、UEFI 补丁、引导配置、文档、工具 | **不想编译就装成品** |

> 只想尽快把机器装回来：**基本不用编译**，直接跳到第五节，用 `packages\` 里的成品逐个 `pnputil` 安装即可。

> ⚠️ **不要把 `erista-0.7.x` 合并进上游默认分支**。它基于 2026-11-08 的快照，
> 相对今天的上游会删掉 SwitchFan(约 6700 行)/SwitchBattery 等新代码。它只用于取回 Erista 这一版。

## 二、构建环境

| 需求 | 说明 |
|---|---|
| Windows 10/11 **x64** 构建机 | 交叉编译 ARM64，不需要 ARM 机器 |
| **Windows Kits 10.0.26100.0**（含 WDK） | Wi-Fi 驱动必须这个版本；22621 缺 WiFiCx/NetAdapterCx 2.2 头文件 |
| **LLVM**（`clang-cl.exe` / `lld-link.exe`） | 默认路径 `C:\Program Files\LLVM\bin` |
| Windows PowerShell 5.1 | 脚本按 5.1 写，不需要 PowerShell 7 |
| Visual Studio / MSVC CRT | **不需要**（主机测试用自带 freestanding shim） |
| 目标机 | 已开测试签名（`testsigning on`）、已信任本文附带的测试证书 |

## 三、取代码

```powershell
git clone https://github.com/dongrez1990-cloud/Windows-for-Switch-Drivers.git
cd Windows-for-Switch-Drivers
```

成品包和文档（不编译只装机就只要这条分支）：

```powershell
git checkout restore/20261118
```

## 四、构建

### 4.1 Wi-Fi 驱动（一条命令）

```powershell
git checkout erista-0.7.x
powershell -File tools\Test-SwitchWifi.ps1                 # 先跑测试
#   期望最后一行：--- 14 test(s): 14 passed, 0 failed ---
powershell -File tools\Build-SwitchWifi.ps1 -OutDir build\SwitchWifi074
#   产物：build\SwitchWifi074\{switchwifi.sys,.inf,.cat}
#   期望 switchwifi.sys = 179112 字节（重新签名后哈希会变，属正常）
```

### 4.2 其它板级驱动（GPIO / I²C / 触屏 / 电源键 / 风扇 / 电池 / RTC / 蓝牙 / UART）

用仓库自带脚本（Windows 原生，参数默认值就是这批驱动需要的 **KMDF 1.15 / NTDDI 0x0A000008**）：

```powershell
powershell -File tools\Build-Driver.ps1 -Name switchpwrbtn `
    -Sources drivers\SwitchPowerButton\src\switchpwrbtn.c, tools\kmdf_support.c `
    -Inf drivers\SwitchPowerButton\switchpwrbtn.inf `
    -Libs vhfkm.lib, libcntpr.lib `
    -OutDir build\switchpwrbtn
```

**每个驱动必须带的类扩展库**（漏了会链接失败 —— 这张表来自上游 `docs/hdh001/build-drivers.sh`）：

| 驱动 | 源码 | `-Libs` | 额外开关 |
|---|---|---|---|
| tegra210gpio | `drivers/Tegra210Gpio/src/tegra210gpio.c` | `msgpioclxstub.lib` | |
| tegra210i2c | `drivers/Tegra210I2c/src/tegra210i2c.c` | `spb/1.0/spbcxstubs.lib` | |
| switchtouch（触屏） | `drivers/SwitchLiteTouch/src/switchtouch.c` | `vhfkm.lib, libcntpr.lib` | |
| switchpwrbtn（电源键） | `drivers/SwitchPowerButton/src/switchpwrbtn.c` | `vhfkm.lib, libcntpr.lib` | |
| switchsio（传感器/音量键） | `drivers/SwitchSensor/src/switchsio.c` | `vhfkm.lib, libcntpr.lib` | |
| switchfan（风扇/背光/时钟） | `drivers/SwitchFan/src/switchfan.c` | 无 | Erista **不要**加 `SWITCH_LITE_GPU` |
| switchbattery（电池） | `drivers/SwitchBattery/src/switchbattery.c` | `battc.lib` | |
| switchrtc | `drivers/SwitchRtc/src/switchrtc.c` | 无 | |
| switchbt（蓝牙） | `drivers/SwitchBluetooth/src/switchbt.c` | 无 | |
| tegraxusb（USB/UART） | `drivers/TegraXusb/src/tegraxusb.c` | 无 | |
| nvtsdhc（SD 卡 UHS） | `drivers/Tegra210Sdhc/src/nvtsdhc.c` | 见 INF | **`-Wdm`**（sdport 小型端口，非 KMDF） |

> 每个驱动还要带上 `tools/kmdf_support.c`（脚本会自动加）。
> 具体 KMDF 版本 / 库路径请以该驱动自己的 `.inf` 为准；`Build-Driver.ps1` 顶部列出了全部参数。

> **Erista 不要加 `SWITCH_LITE_*` 宏**（那是 Lite 专用：`SWITCH_LITE_TOUCH=1`、`SWITCH_LITE_GPU=1`）。
> 上游的 `docs/hdh001/build-drivers.sh` 是 Lite 版，直接照抄会在 Erista 上编出不适用的时钟/触摸路径。

### 4.3 UEFI 引导层（WinX → `bl33.bin`）

```powershell
git clone https://github.com/ruje0504-2/winx.git
cd winx
git checkout hdh001-hekate-sd
git am C:\path\to\restore-20261118\patches\0001-UEFI-WinX-Erista-*.patch
# 用 EDK2 + ARM64 工具链按 NintendoSwitch.dsc 编译，产物即新的 bl33.bin
```

或者直接从本仓库取整套改动：`git fetch git@github.com:dongrez1990-cloud/Windows-for-Switch-Drivers.git uefi/erista`

### 4.4 手柄（Joy-Con）驱动

源码在 `restore/20261118` 分支的 `sources\switchjoycon\`（含 `src`、`tools`、`README.md`、`HANDOFF.md`），
成品是 `packages\switchjoycon-0.3.20.0\`。它**不在**任何上游仓库里，是独立项目。

## 五、签名与安装

### 5.1 证书与测试签名

```powershell
# 目标机上（管理员）
Import-Certificate -FilePath .\tools\cert\SwitchWOA-Test.cer -CertStoreLocation Cert:\LocalMachine\Root
Import-Certificate -FilePath .\tools\cert\SwitchWOA-Test.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
bcdedit /set testsigning on
```

### 5.2 装驱动

```powershell
pnputil /add-driver C:\SwitchWOA\drivers\switchwifi-074\switchwifi.inf /install
```

**铁律（全是踩过的坑）**

1. `pnputil` **只升不降**。回退必须"同一个二进制 + 更高 DriverVer + 重签 cat"
   —— 现成脚本 `tools\pack-switchwifi-rollback.ps1`，现成回退包 `packages\switchwifi-rollback-*`。
2. **绝不**在设备管理器里"卸载设备 + 扫描硬件改动"——会卡死 PnP/服务线程。
3. 装 Wi-Fi 驱动会重启网卡，而 Wi-Fi 往往就是你的 SSH 通道 → 用 **SYSTEM 计划任务**执行（脚本在 `tools\deploy-scripts\`）。
4. 强制关机代价极高（脏位→索引损坏→配置丢失→蓝屏）。能等就等。

### 5.3 验证

```powershell
(Get-PnpDevice -InstanceId 'ACPI\SWOA0D01\0').Status                                  # OK
(Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0D01\0' -KeyName DEVPKEY_Device_DriverVersion).Data  # 0.7.4.0
Get-NetAdapter -Name WLAN | Select Status,LinkSpeed                                   # Up / 866.7 Mbps
# 驱动内部诊断计数器
(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\SWOA0D01\0\Device Parameters' -Name SwitchWifiReport).SwitchWifiReport
```

`SwitchWifiReport` 里几个关键字段：`erx`（收到的 EAPOL 帧）、`kadd`（安装的密钥）、
`kfail`（密钥失败，应恒为 0）、`dn`（断链事件，末段 `E0` 表示下次关联前的固件复位）、`leave`（断连阶段）。
正常重连时 `erx`/`kadd` 应逐次递增；若它们**冻住不动**，就是"链路上不来"的典型症状。

## 六、★ GPU 驱动更新之后怎么重建

GPU 不在本仓库，在 **`https://github.com/ruje0504-2/mesa-switch-woa`**（Mesa / NVK /
`tegra-x1-wddm` WDDM miniport），构建与板级集成见其中 `tegra-x1-wddm/docs/PORTING.md`。

**关键耦合点：`switchfan` 同时是 NVDEC 的时钟提供者。** 所以 GPU 侧更新后，风扇驱动要不要跟着重编，
取决于你用的是哪条时钟耦合：

| 你的情况 | GPU 更新后要做什么 |
|---|---|
| **Lite（上游主线）** | 上游 `build-drivers.sh` 编 `SwitchFan` 时必须带 `SWITCH_LITE_GPU=1` 和 `MESA_TEGRA=<mesa-switch-woa/tegra-x1-wddm 路径>`；GPU 更新后**必须重编 `switchfan`**，否则 NVDEC 拿到的时钟提供者标识与 GPU 侧记录不一致。同时更新 GPU 文档里记录的组件哈希（`NVDEC_INTEGRATION_*.json` 那类回执） |
| **Erista（本文这条线）** | 本线 `switchfan` **不含** `SWITCH_LITE_GPU`/Mesa 头文件耦合（已核对：0 处引用），所以 GPU 包更新后**不需要重编板级驱动**；重装 GPU 包 → 重测即可 |

**通用检查清单（任何机型、任何一次 GPU 更新后都跑一遍）**

1. 回读驱动版本与设备状态：`Get-PnpDevice`、`DEVPKEY_Device_DriverVersion`，确认新 GPU 包已生效、无 Code 10/43。
2. 记录新 GPU 包的哈希（`Get-FileHash`），并在自己的记录里替换旧值 —— 便于以后"哪一版能跑"的追溯。
3. 跑最小图形验收：D3D 基础渲染、Vulkan（如 `vkcube`）各一次，确认不黑屏、能正常退出。
4. 看 `switchfan` 的计数器：`CpuActualKhz` / `RamActualKhz` / `TempSoC` / `Duty` / `SensorFailures` 是否正常
   （GPU 重负载会通过 EMC/时钟策略间接影响它们）。
5. Wi-Fi 侧顺手复测一次断连重连：GPU 驱动更新会重启图形栈，某些机型会牵动电源状态切换，
   确认 `erx`/`kadd` 仍在增长、`kfail` 仍为 0。

## 七、排错

| 症状 | 原因 / 处理 |
|---|---|
| 重连不上、必须禁用/启用网卡 | 旧版驱动在重连后把落在旧 RX 缓冲区上的 EAPOL 丢弃，4 次握手超时。**本文的 0.7.4 已修**（改为按到达时刻记 epoch） |
| `scp` 报 `banner exchange` 超时，但 `ssh` 能用 | 远端 sftp 子系统问题。取文件改用 `ssh host "powershell -EncodedCommand <base64>"` |
| 驱动装完设备变 Code 10 | 签名未受信 / 未开 `testsigning`；或装的是别的分支的包，版本号更低（`pnputil` 只升不降） |
| Wi-Fi 驱动编译报缺 `wificx.h` | WDK 用了 22621；必须 10.0.26100.0 |
| 主机测试 `test_control` 报访问冲突 | 测试运行器缺 x64 栈探测（`__chkstk`）；本文的 `tools\HostTests\chkstk.c` 已提供 |

## 八、版本清单（作者机器实测，2026-11-18）

| 驱动 | 版本 | InfName |
|---|---|---|
| **switchwifi** | **0.7.4.0** | oem52.inf |
| switchbt | 1.0.0.19 | oem15.inf |
| switchfan | 1.0.0.22 | oem17.inf |
| **switchjoycon** | **0.3.20.0** | oem50.inf |
| switchaudio | 1.0.0.17 | oem12.inf |
| switchsio | 0.2.3.0 | oem11.inf |
| switchsensor | 1.0.0.1 | oem10.inf |
| switchtouch | 1.0.0.4 | oem2.inf |
| SwitchBattery | 1.0.0.11 | oem5.inf |
| SwitchPowerButton | 1.0.0.4 | oem3.inf |
| SwitchRtc | 1.0.0.2 | oem9.inf |
| Tegra210Sdhc | 0.4.0.1 | — |
| Tegra210Gpio / I2c | 1.0.0.4 / 1.0.0.5 | oem0 / oem1 |

---

## 九、想用上游最新版？

本文的 `erista-0.7.x` 里，除 Wi-Fi / 电源键 / 音频 / SD 之外，其余驱动是 **2026-11-08 的状态**。
如果你要"上游最新 + Erista 适配"，走这条路：

```powershell
git clone https://github.com/ruje0504-2/Windows-for-Switch-Drivers.git
cd Windows-for-Switch-Drivers
git checkout hdh001-winarm-bounded-write          # 上游最新（2d82f24）
# 只从 Erista 分支取需要的子目录：
git checkout erista-0.7.x -- drivers/SwitchWifi
git am C:\path\to\UEFI-Erista.patch                # UEFI 改动
```

注意：上游 `SwitchFan`/`SwitchBattery` 新代码带 Lite 专用路径，Erista 上需要逐项核对
（`SWITCH_LITE_*` 宏、T214 时钟树、`t214/` 头文件）。

---

**许可证**：驱动部分沿用上游 **GPL-2.0**（见各源文件 SPDX 头）；手柄驱动与本文档为作者自用工程记录。
