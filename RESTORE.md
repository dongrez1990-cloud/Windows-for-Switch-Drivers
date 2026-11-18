# 恢复包 2026-11-18 —— 从 GitHub 重建这台 Switch 的全部状态

> 目标：**本地机器全丢也不怕**。这里的文件 + 你 fork 上的三条分支，足以把
> 初代机（Erista）这台 Switch 上跑的整套东西重建回 2026-11-18 的状态：
> Wi-Fi、蓝牙、电源管理、SD 卡、手柄（Joy-Con）、触屏、以及 UEFI 引导层。

---

## 一、全部内容都保存在哪（总表）

你 fork 的仓库：**`github.com/dongrez1990-cloud/Windows-for-Switch-Drivers`**

| 分支 / 位置 | 覆盖的内容 | 说明 |
|---|---|---|
| **`erista-0.7.x`**<br>tag `erista-0.7.4.0` | **Wi-Fi 驱动**（switchwifi 0.7.2→**0.7.4**）<br>**电源管理**：`SwitchPowerButton` +169 行<br>**音频**：`SwitchAudio` +256 行<br>**SD 卡**：`Tegra210Sdhc` UHS<br>14 个主机测试 + 构建工具 | 基于 11-08 的旧快照，**只作留档，绝不要合并进默认分支**（会删掉上游 SwitchFan/SwitchBattery 等新代码） |
| **`uefi/erista`** | **UEFI 引导层（WinX / EDK2）**：<br>· `WifiBtApp` Wi-Fi+**蓝牙** 初始化、`t210/` 初代机新阶段表、`WifiLog.h`<br>· `PlatformBootManagerLib/WifiHandoff.c`（UEFI→Windows 交棒，UEFI 阶段把固件 RSS/CLM 交给驱动）<br>· `t214/power.h` + `AcpiTables/Dsdt.asl`（**电源/ACPI**）<br>· `SdMmcDxe/SdMmc.c`（SD）<br>· `NintendoSwitch.dsc` 平台构建描述 | **16 个文件，+637/−86**，此前全部未提交。含上游我方那 5 个未推送提交 |
| **`restore/20261118`** | **本包**：不在任何仓库里的项目源码、各驱动已签名成品、文档、工具、引导配置 | 见下方第二节 |
| 朋友仓库（fork 自带） | **触屏** `SwitchTouch`、**蓝牙** `SwitchBluetooth`、风扇、电池、CPU 时钟等驱动 | 这些我们没改，沿用朋友上游版本；fork 里已有 |

> **触屏（switchtouch 1.0.0.4）和蓝牙的 Windows 侧驱动（switchbt 1.0.0.19）我们没有改动**，
> 就是朋友上游的原版；fork 里已完整复制。UEFI 侧的蓝牙初始化（`WifiBt.c`）是我们的改动，在 `uefi/erista`。

## 二、本包（`restore/20261118`）里有什么

| 目录 | 内容 |
|---|---|
| `RESTORE.md` | 本文 |
| `sources\` | `switchjoycon`（**手柄驱动**，含 src/tools/README/HANDOFF）、`SwitchPad`（安卓上位机）、`SwitchKey` —— **这三个项目不在任何仓库里** |
| `packages\` | **17 套已签名成品**（每套 `.sys/.inf/.cat`），版本与机器实际安装的逐一核对一致：<br>switchwifi **0.7.4** / 0.7.2 / 回退包(0.7.0.0 二进制) · switchjoycon **0.3.20**<br>switchbt 1.0.0.19 · switchfan 1.0.0.22 · switchaudio 1.0.0.17 · switchbattery 1.0.0.11<br>switchpwrbtn 1.0.0.4 · switchrtc 1.0.0.2 · switchsio 0.2.3.0 · switchsensor 1.0.0.1<br>switchtouch 1.0.0.4 · tegra210gpio 1.0.0.4 · tegra210i2c 1.0.0.5 · tegra210sdhc 0.4.0.1 | 不重新编译也能把整台机器装回来 |
| `patches\` | `0001-UEFI-WinX-Erista-...patch` —— UEFI 那 16 个文件的完整补丁（`git am` 可直接应用） |
| `esp\` | 引导配置：`startup.nsh`、`bootloader\hekate_ipl.ini`、`bootloader\ini\*.ini`（各启动项）<br>`MANIFEST-binaries-sha256.txt`：44 个引导二进制的哈希清单（**只记哈希不装文件**，重新获取后可比对） |
| `docs\` | 交接文档、0.7.3/0.7.4 报告、基线清单、上机日志、故障记录、**实测驱动版本清单** |
| `tools\` | 构建/测试/签名脚本、`HostTests\`、测试证书、`wdk-patch\`、上机部署脚本、回退包打包脚本 |

## 三、重建环境

| 需求 | 说明 |
|---|---|
| Windows 10/11 **x64** | 交叉编译 ARM64，不需要 ARM 机器 |
| **Windows Kits 10.0.26100.0**（含 WDK） | Wi-Fi 驱动必须这个版本；22621 的 WiFiCx 头文件编不过 |
| **LLVM**（`clang-cl.exe`/`lld-link.exe`）在 `C:\Program Files\LLVM\bin` | 构建 + 主机测试 |
| Windows PowerShell 5.1 | 本机没有 PowerShell 7 |
| ~~Visual Studio / MSVC CRT~~ | **不需要**：主机测试走 freestanding shim |
| 重建 UEFI 才需要 | EDK2 + ARM64 工具链（devkitA64 / GCC），以及 hekate |
| 目标机 | 测试签名模式 + 信任 `tools\cert\SwitchWOA-Test.cer` |

## 四、逐步重建

### 4.1 Windows 驱动（Wi-Fi / 电源 / 音频 / SD / 手柄）

```powershell
git clone git@github.com:dongrez1990-cloud/Windows-for-Switch-Drivers.git
cd Windows-for-Switch-Drivers
git checkout erista-0.7.x

# 主机测试：14 个用例，不需要上机
powershell -File tools\Test-SwitchWifi.ps1
#   期望：--- 14 test(s): 14 passed, 0 failed ---

# 编 Wi-Fi 驱动
powershell -File tools\Build-SwitchWifi.ps1 -OutDir build\SwitchWifi074
#   期望 switchwifi.sys = 179112 字节（重签名后哈希会变）
```

**手柄驱动**：源码在 `sources\switchjoycon\`，构建脚本 `sources\switchjoycon\tools\`。
**懒得编译**：`packages\` 里全是已签名成品，可直接装。

### 4.2 UEFI 引导层（WinX）

```powershell
git clone https://github.com/ruje0504-2/winx.git          # 朋友的上游
cd winx
git am C:\path\to\restore-20261118\patches\0001-UEFI-WinX-*.patch
# 然后用 EDK2 工具链按 NintendoSwitch.dsc 编译出 bl33.bin
```

也可以直接从你的 fork 取：`git fetch git@github.com:dongrez1990-cloud/Windows-for-Switch-Drivers.git uefi/erista`

### 4.3 引导区（ESP）

用 `esp\` 里的 `startup.nsh` + `bootloader\ini\*.ini` 还原启动项；
`bl33.bin`、`bootmgfw.efi`、hekate 负载等二进制按 `esp\MANIFEST-binaries-sha256.txt`
从 hekate 发布页 / Windows 安装介质 / 自己的 UEFI 构建取回，并用哈希核对。

## 五、装回 Switch

```powershell
scp -i <密钥> packages\switchwifi-0.7.4.0\* woa@<Switch-IP>:C:/SwitchWOA/drivers/switchwifi-074/
# 装驱动会重启网卡，而 WiFi 就是 SSH 通道 → 用 SYSTEM 计划任务执行（脚本见 tools\deploy-scripts\）
pnputil /add-driver C:\SwitchWOA\drivers\switchwifi-074\switchwifi.inf /install
```

**铁律**

1. `pnputil` **只升不降**：回退必须"同一二进制 + 更高 DriverVer + 重签 cat"（`tools\pack-switchwifi-rollback.ps1`，现成回退包在 `packages\switchwifi-rollback-*`）。
2. **绝不**在设备管理器里"卸载设备 + 扫描硬件改动"。
3. 会掉网的操作全部走 SYSTEM 计划任务 + 写日志，事后回读。
4. `scp` 的 sftp 子系统会挂（banner 超时），但 `ssh` 能通 → 取日志用 `ssh ... "powershell -EncodedCommand <base64>"`。
5. 强制关机代价极高（脏位→索引损坏→配置丢失→蓝屏）。

## 六、当天机器上的驱动版本（实测）

| 驱动 | 版本 | InfName |
|---|---|---|
| **switchwifi** | **0.7.4.0** | oem52.inf |
| switchbt（蓝牙） | 1.0.0.19 | oem15.inf |
| switchfan | 1.0.0.22 | oem17.inf |
| **switchjoycon（手柄）** | **0.3.20.0** | oem50.inf |
| switchaudio | 1.0.0.17 | oem12.inf |
| switchsio | 0.2.3.0 | oem11.inf |
| switchsensor | 1.0.0.1 | oem10.inf |
| switchtouch（触屏） | 1.0.0.4 | oem2.inf |
| SwitchBattery | 1.0.0.11 | oem5.inf |
| SwitchPowerButton | 1.0.0.4 | oem3.inf |
| SwitchRtc | 1.0.0.2 | oem9.inf |
| Tegra210Sdhc | 0.4.0.1 | — |
| Tegra210Gpio / I2c | 1.0.0.4 / 1.0.0.5 | oem0 / oem1 |

Wi-Fi 实测：`Up 866.7 Mbps`，5 GHz ch36，802.11ac；连续 5 次断连重连 + 10 轮浸泡全部成功，
全程无需禁用/启用网卡。

## 七、这次 Wi-Fi 到底改了什么

- **0.7.3.0**：移植上游 Lite 0.5.7 的断连策略（`ConfirmLinkDown` 回读 BSSID 后才判死 + `leave=phase/status` 上报）。
- **0.7.4.0**：修 `RxComplete()` 的 epoch 判定 —— 重连后 EAPOL M1 落在旧空缓冲区上会被当过期包丢弃，导致 4 次握手超时、必须禁用/启用网卡。改为按**到达时刻**记 epoch。
- 详细证据链见 `docs\WIFI-073-074-REPORT.md`。
- 注意：这两处上游 `2d82f24` 其实已经有了；我们这棵树从 11-08 旧快照分出，属于重新推导。所以 `erista-0.7.x` 只作留档。
