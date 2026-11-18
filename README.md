# 初代 Switch（Erista / T210）Windows 驱动全套装 —— 装完即用

> 这套东西能让**初代 Nintendo Switch（HAC-001，Tegra X1 / T210 / Erista）**跑起完整的
> Windows 11 ARM64 驱动栈：Wi-Fi、蓝牙、电源管理、风扇/背光、电池、触屏、手柄（Joy-Con）、
> SD 卡、音频、GPIO/I²C。
>
> ⚠️ **只适用于初代 HAC-001**。续航版（V2）、Lite、OLED 都是 Mariko（T210B01 / T214），
> 代际不同，见下方「适用机型」。
>
> 作者机器实测（2026-11-18）：13 个驱动全部正常，Wi-Fi `Up 866.7 Mbps`，
> 连续 5 次断连重连 + 10 轮浸泡全部自动恢复，**全程无需禁用/启用网卡**。

## ⚡ 快速开始

```powershell
git clone https://github.com/dongrez1990-cloud/Windows-for-Switch-Drivers.git
cd Windows-for-Switch-Drivers
git checkout restore/20261118
```

然后照 **[INSTALL.md](INSTALL.md)** 做（备 SD 卡 → 装 Windows → 导证书 → 一条命令装驱动 → 验证）。

**一句话**：`install\Install-SwitchDrivers.ps1` 一个脚本把三张证书 + 13 个驱动按正确顺序装完并自检。
**不需要编译任何东西。**

## 适用机型（重要，别装错）

| 机型 | 型号 | SoC | 本套东西 |
|---|---|---|---|
| **初代 Switch**（2017） | **HAC-001** | **Tegra X1 / T210**（Erista） | ✅ **就是在本机实测的**（SMBIOS 自报 `HAC-001` / `Tegra X1` / BIOS `winx-r402`） |
| 续航版 V2（2019） | HAC-001(-01) | Tegra X1+ / T210B01（**Mariko**） | ⚠️ **未实测**。SoC 换了代，别直接套用 |
| Switch Lite | HDH-001 | T210B01（**Mariko**） | ⚠️ 未实测。**上游就是为它写的** → 直接用 [`ruje0504-2/Windows-for-Switch-Drivers`](https://github.com/ruje0504-2/Windows-for-Switch-Drivers) 的 `docs/hdh001/` |
| Switch OLED（2021） | HEG-001 | T210B01（**Mariko**） | ⚠️ **未实测**。除了 SoC，面板 / 背光 / 底座 / 64 GB eMMC 都与 Lite、初代不同，属于**独立移植课题**，不能直接套 |

> **一句话**：本文只对 **HAC-001（初代 / T210 / Erista）** 负责。
> **续航版、Lite、OLED 都是 Mariko（T210B01 / T214）**，与本机代际不同 ——
> 其中 Lite 有上游现成支持，续航版和 OLED 目前没有经过验证的公开方案。
>
> 这也是为什么本仓库的分支叫 `erista-*`：它做的是**把上游那套 Lite(T214) 代码搬回初代(T210)**，
> 方向恰好相反，不能倒着用。

## 三条分支

| 分支 | 内容 | 什么时候用 |
|---|---|---|
| **`restore/20261118`**（就是本分支） | 17 套已签名驱动包 · UEFI 成品固件（`bl31/bl33`）· hekate 负载 · 引导配置 · 一键安装脚本 · 三张公钥证书 · 文档 | **装机（默认走这里）** |
| `erista-0.7.x`（tag `erista-0.7.4.0`） | 驱动源码 + 构建脚本 + 14 个主机测试，**干净克隆即可编译** | 改代码 / 自己造包 |
| `uefi/erista` | UEFI 引导层（WinX / EDK2）改动：Wi-Fi+蓝牙初始化、`t210/` 阶段表、ACPI/电源表、SD、`WifiHandoff` | 改 UEFI |

## 你需要做什么（三类）

| 类别 | 内容 | 要做什么 |
|---|---|---|
| **装**（13 个驱动 + 固件） | 全部已签名成品，含 UEFI `bl33.bin` | 跑 `Install-SwitchDrivers.ps1`，见 [INSTALL.md](INSTALL.md) |
| **可能要自己编译** | 只有改代码、或想改 UEFI 固件时 | 见 [BUILD.md](BUILD.md) |
| **必须自己准备** | Windows 11 ARM64 镜像、SD 卡、hekate 启动方式 | 仓库不提供，也不该提供 |

> 那 4 个由证书 `A9E1F054…` 签名的驱动（触屏 / RTC / 传感器 / GPIO）：
> 该证书**私钥已不存在**，但公钥证书已随包提供（`install\certs\`），
> 安装脚本会自动导入信任，**所以也不需要重编**。

## 包里有什么

```
restore-20261118/
├─ INSTALL.md              装机指南（先看这个）
├─ BUILD.md                从源码构建 + GPU 驱动更新后的重建
├─ RESTORE.md              灾难恢复手册（作者视角：整机全丢怎么回来）
├─ packages/               17 套已签名驱动（.sys/.inf/.cat），版本与作者机器逐字节核对
├─ firmware/               winx-v39/{bl31,bl33}.bin + hekate_ctcaer_6.5.5.bin
├─ esp/                    startup.nsh + hekate_ipl.ini + 41 个启动项 ini
├─ install/                Install-SwitchDrivers.ps1 + certs/（三张公钥证书）
├─ sources/                switchjoycon（手柄驱动源码）· SwitchPad · SwitchKey
├─ patches/                UEFI（WinX）改动补丁，git am 可直接应用
├─ tools/                  构建/测试/签名脚本 + 上机部署脚本
└─ docs/                   报告、日志、基线清单、实测驱动版本表
```

## 期望版本（作者机器实测）

| 驱动 | 版本 | | 驱动 | 版本 |
|---|---|---|---|---|
| **switchwifi** | **0.7.4.0** | | switchbattery | 1.0.0.11 |
| **switchjoycon** | **0.3.20.0** | | switchpwrbtn | 1.0.0.4 |
| switchbt | 1.0.0.19 | | switchrtc | 1.0.0.2 |
| switchfan | 1.0.0.22 | | switchsio | 0.2.3.0 |
| switchaudio | 1.0.0.17 | | switchsensor | 1.0.0.1 |
| switchtouch | 1.0.0.4 | | tegra210gpio / i2c | 1.0.0.4 / 1.0.0.5 |
| nvtsdhc（SD） | 0.4.0.1 | | | |

## 上游与致谢

本仓库是**二次开发的分支归档**，底层工作几乎全部来自下面这些公开项目。
**遇到驱动行为问题请先查上游文档**；本仓库只多做了一件事：**Erista（T210）适配 + 可直接安装的成品**。

| 项目 | 地址 | 在本套东西里的作用 |
|---|---|---|
| **Windows-for-Switch-Drivers**<br>（上流主线，面向 Lite） | https://github.com/ruje0504-2/Windows-for-Switch-Drivers | **真正的上游**：全部驱动源码、`docs/hdh001/` 下的构建与上机文档。本仓库 `erista-0.7.x` 就是从它的一个快照分出来的 |
| **winx**（UEFI / EDK2 引导层） | https://github.com/ruje0504-2/winx | WinX 固件本体：ACPI 表、`WifiBtApp`（Wi-Fi+蓝牙初始化）、`PlatformBootManagerLib`；`bl33.bin` 就从这里编译出来 |
| **mesa-switch-woa**（GPU / 图形 / 视频） | https://github.com/ruje0504-2/mesa-switch-woa | Mesa / NVK + `tegra-x1-wddm` 的 WDDM miniport。`switchfan` 的 NVDEC 时钟耦合就在这里 |
| **winx 的更上游**（NintendoSwitchPkg 系） | https://github.com/steck0714/winx | WinX 本身是它的分支（改用 2024 版 EDK2） |
| **hekate**（引导器） | https://github.com/CTCaer/hekate | `firmware\hekate\hekate_ctcaer_6.5.5.bin` 是它的发布物；SD 卡 `bootloader\` 结构也来自它 |
| **Atmosphère** | https://github.com/Atmosphere-NX/Atmosphere | 用 Atmosphère 方式引导时需要 |
| **EDK2** | https://github.com/tianocore/edk2 | 只有自己重建 `bl33.bin` 时才需要 |
| 原始项目（仅追溯） | `hayato41200/Windows-for-Switch-Drivers` | 上游 README 注明：共享驱动实现、SwitchCenter、SwitchPatcher 源自该项目。**该地址目前已不可访问**（账号可能改名或删除），所以只记名字、不挂死链 |

> 以上除最后一条外，链接在 2026-11-18 均实测可访问。

## 许可

- **驱动 / 固件代码**：沿用上游 **GPL-2.0**（见各源文件 SPDX 头）。
- **本仓库提供的成品二进制**（`packages\`、`firmware\`）都由上述 GPL-2.0 源码编译而来，
  对应源码在 `erista-0.7.x`、`uefi/erista` 两个分支与 `patches\` 目录中。
- **手柄驱动（switchjoycon）与本文档**：作者自用工程记录。

## 关于本仓库的定位

这是作者的**备份分支**，不是上游的替代品，也不打算合并进上游默认分支
（`erista-0.7.x` 基于 2026-11-08 快照，相对今天的上游会缺少 SwitchFan/SwitchBattery 等新代码）。
想参与上游开发请直接去上面第一个链接。
