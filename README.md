# 初代 Switch（Erista / T210）Windows 驱动全套装 —— 装完即用

> 这套东西能让**初代 Nintendo Switch / OLED（Tegra X1 / T210）**跑起完整的 Windows 11 ARM64 驱动栈：
> Wi-Fi、蓝牙、电源管理、风扇/背光、电池、触屏、手柄（Joy-Con）、SD 卡、音频、GPIO/I²C。
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

## 适用机型

| 机型 | 代号 | 本文适用 |
|---|---|---|
| 初代 Switch / OLED（Tegra X1） | **Erista / T210** | ✅ |
| Switch Lite（Tegra X1+） | HDH-001 / T214 | ❌ 请走上游 [`ruje0504-2/Windows-for-Switch-Drivers`](https://github.com/ruje0504-2/Windows-for-Switch-Drivers) 的 `docs/hdh001/` |

上游主线是**为 Lite 写的**（`SWITCH_LITE_TOUCH` / `SWITCH_LITE_GPU` / T214 时钟树）。
本仓库这三条分支是把它**适配到 Erista** 的成果。

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

## 许可

驱动代码沿用上游 **GPL-2.0**（见各源文件 SPDX 头）；固件成品（`bl33.bin` 等）同样来自 GPL-2.0 源码，
对应源码在 `uefi/erista` 分支与 `patches/` 里。手柄驱动与文档为作者自用工程记录。
