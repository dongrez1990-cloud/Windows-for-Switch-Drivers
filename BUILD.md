# 从源码构建 —— 初代 Switch（Erista / T210）

> 只想**装**：看 [INSTALL.md](INSTALL.md)，那里面全是现成成品，不需要编译。
> 本文只在你要**改代码 / 造自己的签名包 / 改 UEFI** 时用。
>
> 上游项目与致谢见 [README.md](README.md)。本文引用的构建约定（`docs/hdh001/`、
> 类扩展库、`SWITCH_LITE_*` 宏）都来自
> [ruje0504-2/Windows-for-Switch-Drivers](https://github.com/ruje0504-2/Windows-for-Switch-Drivers)，
> UEFI 部分来自 [ruje0504-2/winx](https://github.com/ruje0504-2/winx)，
> GPU 部分来自 [ruje0504-2/mesa-switch-woa](https://github.com/ruje0504-2/mesa-switch-woa)。

---

## 0. 构建环境

| 需求 | 说明 |
|---|---|
| Windows 10/11 **x64** 构建机 | 交叉编译 ARM64，不需要 ARM 机器 |
| **Windows Kits 10.0.26100.0**（含 WDK） | Wi-Fi 驱动必须这个版本；22621 的 WiFiCx 缺 `WIFI_STATION_CAPABILITIES_INIT` 和 RSNA AKM 字段，编不过 |
| **LLVM**（`clang-cl.exe` / `lld-link.exe`） | 默认路径 `C:\Program Files\LLVM\bin` |
| Windows PowerShell 5.1 | 脚本按 5.1 写；不需要 PowerShell 7 |
| Visual Studio / MSVC CRT | **不需要**（主机测试用仓库自带的 freestanding shim） |
| 仅重建 UEFI 时需要 | EDK2 + ARM64 工具链 |

取代码：

```powershell
git clone https://github.com/dongrez1990-cloud/Windows-for-Switch-Drivers.git
cd Windows-for-Switch-Drivers
git checkout erista-0.7.x        # 驱动源码 + 构建脚本
```

## 1. Wi-Fi 驱动

```powershell
powershell -File tools\Test-SwitchWifi.ps1
#   期望最后一行：--- 14 test(s): 14 passed, 0 failed ---
powershell -File tools\Build-SwitchWifi.ps1 -OutDir build\SwitchWifi074
#   产物：build\SwitchWifi074\{switchwifi.sys,.inf,.cat}
#   期望 switchwifi.sys = 179112 字节（重新签名会改哈希，属正常）
```

> 这个分支**自包含**：干净克隆就能编，不依赖仓库外的任何脚本
> （`tools\Patch-NdisHeader.ps1` 已随仓库提供，且只在 WDK < 26100 时才需要）。

## 2. 其它板级驱动

```powershell
powershell -File tools\Build-Driver.ps1 -Name switchpwrbtn `
    -Sources drivers\SwitchPowerButton\src\switchpwrbtn.c, tools\kmdf_support.c `
    -Inf drivers\SwitchPowerButton\switchpwrbtn.inf `
    -Libs vhfkm.lib, libcntpr.lib `
    -OutDir build\switchpwrbtn
```

**每个驱动必须带的类扩展库**（漏了会链接失败；这张表来自上游 `docs/hdh001/build-drivers.sh`）：

| 驱动 | 源码 | `-Libs` | 额外 |
|---|---|---|---|
| tegra210gpio | `drivers/Tegra210Gpio/src/tegra210gpio.c` | `msgpioclxstub.lib` | |
| tegra210i2c | `drivers/Tegra210I2c/src/tegra210i2c.c` | `spb/1.0/spbcxstubs.lib` | |
| switchtouch | `drivers/SwitchLiteTouch/src/switchtouch.c` | `vhfkm.lib, libcntpr.lib` | |
| switchpwrbtn | `drivers/SwitchPowerButton/src/switchpwrbtn.c` | `vhfkm.lib, libcntpr.lib` | |
| switchsio | `drivers/SwitchSensor/src/switchsio.c` | `vhfkm.lib, libcntpr.lib` | |
| switchfan | `drivers/SwitchFan/src/switchfan.c` | 无 | Erista **不要**加 `SWITCH_LITE_GPU` |
| switchbattery | `drivers/SwitchBattery/src/switchbattery.c` | `battc.lib` | |
| switchrtc | `drivers/SwitchRtc/src/switchrtc.c` | 无 | |
| switchbt | `drivers/SwitchBluetooth/src/switchbt.c` | 无 | |
| tegraxusb | `drivers/TegraXusb/src/tegraxusb.c` | 无 | |
| nvtsdhc（SD） | `drivers/Tegra210Sdhc/src/nvtsdhc.c` | 见 INF | **`-Wdm`** |

> **Erista 不要加 `SWITCH_LITE_*` 宏**（`SWITCH_LITE_TOUCH=1`、`SWITCH_LITE_GPU=1` 是 Lite 专用）。
> 上游 `docs/hdh001/build-drivers.sh` 是 Lite 版，照抄会在 Erista 上编出不适用的时钟/触摸路径。

### 2.1 自己签名

`Build-Driver.ps1` / `Build-SwitchWifi.ps1` 会从 `Cert:\CurrentUser\My` 里按
`CN=SwitchWOA Test Driver Signing` 找证书签名。你有两种选择：

- **用你自己的证书**：`New-SelfSignedCertificate -Subject 'CN=SwitchWOA Test Driver Signing' -Type CodeSigningCert -CertStoreLocation Cert:\CurrentUser\My`
  然后把它导出成 `.cer` 在目标机上导入信任（`INSTALL.md` 第 3 节同样流程）；
- **或者**从作者那里拿到 `KEY-BACKUP` 里的 PFX（私钥不外传，这条一般走不通）。

签完还要生成 cat：

```powershell
& "$env:ProgramFiles(x86)\Windows Kits\10\bin\10.0.26100.0\x86\Inf2Cat.exe" /driver:<包目录> /os:10_NI_ARM64 /uselocaltime
```

## 3. UEFI 引导层（WinX → `bl33.bin`）

```powershell
git clone https://github.com/ruje0504-2/winx.git
cd winx
git checkout hdh001-hekate-sd
git am C:\path\to\restore-20261118\patches\0001-UEFI-WinX-Erista-*.patch
# 用 EDK2 + ARM64 工具链按 NintendoSwitch.dsc 编译，产物即新的 bl33.bin（连同 bl31.bin）
```

也可以直接从本仓库取整套改动：
`git fetch git@github.com:dongrez1990-cloud/Windows-for-Switch-Drivers.git uefi/erista`

> 已经给了成品固件（`firmware\winx-v39\`），**只有想改 UEFI 代码时才需要这一步**。

## 4. 手柄（Joy-Con）驱动

源码在 `restore/20261118` 分支的 `sources\switchjoycon\`（`src`、`tools`、`README.md`、`HANDOFF.md`），
成品是 `packages\switchjoycon-0.3.20.0\`。它**不在**任何上游仓库里，是独立项目。

## 5. ★ GPU 驱动更新之后怎么重建

GPU 不在本仓库，在 **`https://github.com/ruje0504-2/mesa-switch-woa`**
（Mesa / NVK / `tegra-x1-wddm` 的 WDDM miniport），构建与板级集成见其中
`tegra-x1-wddm/docs/PORTING.md`。

**关键耦合点：`switchfan` 同时是 NVDEC 的时钟提供者。**

| 你的情况 | GPU 更新后要做什么 |
|---|---|
| **Lite（上游主线）** | 上游 `build-drivers.sh` 编 `SwitchFan` 时必须带 `SWITCH_LITE_GPU=1` 和 `MESA_TEGRA=<mesa-switch-woa/tegra-x1-wddm>`；GPU 更新后**必须重编 `switchfan`**，并更新 GPU 文档里记录的组件哈希（`NVDEC_INTEGRATION_*.json` 那类回执） |
| **Erista（本文这条线）** | 本线 `SwitchFan` **不含** `SWITCH_LITE_GPU`/Mesa 头文件耦合（已核对：0 处引用）→ GPU 包更新后**不需要重编板级驱动**，重装 GPU 包 + 重测即可 |

**每次 GPU 更新后的通用检查清单**

1. 回读驱动版本与设备状态（`Get-PnpDevice`、`DEVPKEY_Device_DriverVersion`），确认新包生效、无 Code 10/43；
2. 记录新 GPU 包的 `Get-FileHash`，替换旧记录，便于追溯"哪一版能跑"；
3. 跑最小图形验收：D3D 基础渲染 + Vulkan（如 `vkcube`）各一次，不黑屏、能正常退出；
4. 看 `switchfan` 计数器：`CpuActualKhz` / `RamActualKhz` / `TempSoC` / `Duty` / `SensorFailures` 是否正常；
5. 顺手复测一次 Wi-Fi 断连重连（`erx`/`kadd` 应继续增长、`kfail` 为 0）。

## 6. 想用上游最新版？

本文这条线的 `erista-0.7.x` 里，除 Wi-Fi / 电源键 / 音频 / SD 之外，其余驱动是 **2026-11-08 的快照**。
要"上游最新 + Erista 适配"：

```powershell
git clone https://github.com/ruje0504-2/Windows-for-Switch-Drivers.git
cd Windows-for-Switch-Drivers
git checkout hdh001-winarm-bounded-write      # 上游最新
git checkout erista-0.7.x -- drivers/SwitchWifi   # 只取 Erista 的 Wi-Fi
git am C:\path\to\UEFI-Erista.patch
```

注意：上游 `SwitchFan` / `SwitchBattery` 新代码带 Lite 专用路径（`SWITCH_LITE_*`、T214 时钟树、
`src/t214/` 头文件），Erista 上需要逐项核对。
