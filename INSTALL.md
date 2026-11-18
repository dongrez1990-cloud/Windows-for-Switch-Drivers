# 安装指南 —— 初代 Switch（Erista / T210）Windows 驱动全套装

> 本文只讲**怎么装**。想自己编译看 [BUILD.md](BUILD.md)；
> 想看完整背景/灾难恢复看 [RESTORE.md](RESTORE.md)。
>
> **这些驱动和固件不是本仓库原创**，上游项目见 [README.md](README.md) 的「上游与致谢」一节，
> 出问题时上游文档（尤其 `ruje0504-2/Windows-for-Switch-Drivers` 的 `docs/hdh001/`）通常更有答案。
>
> 目标机器实测状态（作者）：Windows 11 ARM64，Wi-Fi `Up 866.7 Mbps`，
> 全部 13 个驱动正常，连续 5 次断连重连 + 10 轮浸泡全部自动恢复。

---

## 0. 准备清单

### 必须有的（仓库里不提供）

| 东西 | 说明 |
|---|---|
| **初代 Switch / OLED**（T210 / Erista） | Lite（HDH-001）不适用本文 |
| **Windows 11 ARM64 镜像** | 你自己的安装介质 |
| **microSD 卡** | 建议 128 GB 以上 |
| **hekate 启动方式** | RCM 注入（USB）或 Atmosphere 引导。负载已在仓库里 |
| 能读 SD 卡的电脑 | 用来放文件 |

### 本仓库提供的（都在 `restore/20261118` 分支）

| 目录 | 内容 |
|---|---|
| `packages\` | **17 套已签名驱动包**（`.sys/.inf/.cat`），版本与作者机器逐字节核对过 |
| `firmware\` | `winx-v39\{bl31.bin, bl33.bin}`（UEFI 成品）、`hekate\hekate_ctcaer_6.5.5.bin` |
| `esp\` | `startup.nsh`、`bootloader\hekate_ipl.ini`、41 个启动项 ini |
| `install\` | **`Install-SwitchDrivers.ps1` 一键安装脚本** + `certs\`（三张公钥证书） |
| `sources\` `patches\` `tools\` | 手柄驱动源码、UEFI 补丁、构建工具（装完再用不到） |

---

## 1. 把 SD 卡准备好

把 SD 卡格式化成 **FAT32**，按下面的结构放文件。
`← 来源` 一列写的是"从本仓库哪个位置拷过来"。

```
SD 卡根目录/
├─ startup.nsh                          ← esp\startup.nsh
├─ bootloader/
│   ├─ hekate_ipl.ini                   ← esp\bootloader\hekate_ipl.ini
│   ├─ ini/
│   │   └─ *.ini                        ← esp\bootloader\ini\*.ini （41 个启动项，可只留自己需要的）
│   └─ payloads/
│       └─ hekate_ctcaer_6.5.5.bin      ← firmware\hekate\hekate_ctcaer_6.5.5.bin
├─ switchroot/
│   └─ winx-v39/
│       ├─ bl31.bin                     ← firmware\winx-v39\bl31.bin
│       └─ bl33.bin                     ← firmware\winx-v39\bl33.bin   （WinX UEFI 本体）
├─ EFI/
│   └─ Microsoft\Boot\bootmgfw.efi      ← 由 Windows 安装过程生成（startup.nsh 会去找它）
└─ SwitchWOA/
    └─ drivers/                         ← 建议把 packages\ 整个拷进来，装驱动时不用再插 U 盘
```

> `bootloader\ini\00-swoa-v39.ini` 里写的是 `l4t=1` 和 `boot_prefixes=switchroot/winx-v39/`，
> 所以 `switchroot\winx-v39\` 这个目录名**不要改**；想换新固件就新建 `winx-v40` 并加一个对应 ini。

## 2. 装 Windows

用你自己的 Windows 11 ARM64 镜像正常安装。装完能进桌面（此时大概率没有 Wi-Fi、没有触屏、
没有声音 —— 这是正常的，驱动还没装）。

从 hekate 选择 `SWOA` 启动项进系统。

## 3. 导入签名证书（必须做，否则驱动装不上）

我们的驱动是**测试签名**的。在 Switch 上开 **管理员 PowerShell**，执行：

```powershell
cd <你放 install 目录的路径>\install
foreach ($f in Get-ChildItem .\certs\*.cer) {
    Import-Certificate -FilePath $f.FullName -CertStoreLocation Cert:\LocalMachine\Root
    Import-Certificate -FilePath $f.FullName -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
}
bcdedit /set testsigning on
# 然后重启一次
```

三张证书分别是：

| 文件 | 用途 |
|---|---|
| `switchwoa-current-A7C0A920.cer` | Wi-Fi / 风扇 / 音频 / 手柄 / SD 驱动 |
| `switchwoa-2020-05C4B7DA.cer` | 蓝牙 / 电池 / 电源键 / 音量 / I²C |
| `switchwoa-stage6-A9E1F054.cer` | **触屏 / RTC / 传感器 / GPIO** ← 这张很关键，见下方说明 |

> **为什么这张证书很重要**：`A9E1F054` 是从作者机器上导出的**公钥**。
> 它的私钥已经不存在于任何机器，所以**签名不可复现** —— 但只要导入这张公钥并信任，
> 那 4 个驱动就能直接装，不需要你自己重新编译。脚本会自动导入它。

## 4. 装驱动

### 方式 A：一键（推荐）

```powershell
# 先干跑看一眼会做什么（不需要管理员）
.\install\Install-SwitchDrivers.ps1 -DryRun

# 正式安装（管理员）
.\install\Install-SwitchDrivers.ps1
```

脚本会：导入三张证书 → 按正确顺序装 13 个驱动（**Wi-Fi 放最后**，因为它会重启网卡）
→ 打印"期望版本 vs 实际版本"核对表 → 写 `install\install.log`。

只装某几个：

```powershell
.\install\Install-SwitchDrivers.ps1 -Only switchwifi,switchfan
```

### 方式 B：手动逐个装

```powershell
pnputil /add-driver C:\SwitchWOA\drivers\tegra210gpio-1.0.0.4\tegra210gpio.inf /install
pnputil /add-driver C:\SwitchWOA\drivers\tegra210i2c-1.0.0.5\tegra210i2c.inf /install
pnputil /add-driver C:\SwitchWOA\drivers\tegra210sdhc-0.4.0.1\nvtsdhc.inf /install
pnputil /add-driver C:\SwitchWOA\drivers\switchtouch-1.0.0.4\switchtouch.inf /install
pnputil /add-driver C:\SwitchWOA\drivers\switchsensor-1.0.0.1\switchsensor.inf /install
pnputil /add-driver C:\SwitchWOA\drivers\switchsio-0.2.3.0\switchsio.inf /install
pnputil /add-driver C:\SwitchWOA\drivers\switchbattery-1.0.0.11\switchbattery.inf /install
pnputil /add-driver C:\SwitchWOA\drivers\switchrtc-1.0.0.2\switchrtc.inf /install
pnputil /add-driver C:\SwitchWOA\drivers\switchpwrbtn-1.0.0.4\switchpwrbtn.inf /install
pnputil /add-driver C:\SwitchWOA\drivers\switchfan-1.0.0.22\switchfan.inf /install
pnputil /add-driver C:\SwitchWOA\drivers\switchaudio-1.0.0.17\switchaudio.inf /install
pnputil /add-driver C:\SwitchWOA\drivers\switchbt-1.0.0.19\switchbt.inf /install
pnputil /add-driver C:\SwitchWOA\drivers\switchjoycon-0.3.20.0\switchjoycon.inf /install
pnputil /add-driver C:\SwitchWOA\drivers\switchwifi-0.7.4.0\switchwifi.inf /install
```

> **顺序是有讲究的**：总线和平台驱动先装（GPIO / I²C / SD），外设其次，
> **Wi-Fi 一定最后** —— 它一装完网卡就重启。

## 5. 验证

```powershell
# 驱动版本总表（对着 INSTALL 里第 6 节的期望值看）
Get-CimInstance Win32_PnPSignedDriver |
    Where-Object { $_.DeviceName -match 'Switch|Tegra|Wireless' } |
    Sort-Object DeviceName | Format-Table DeviceName, DriverVersion, InfName -AutoSize

# Wi-Fi 专项
(Get-PnpDevice -InstanceId 'ACPI\SWOA0D01\0').Status                       # OK
Get-NetAdapter -Name WLAN | Select-Object Status, LinkSpeed                # Up / 866.7 Mbps

# Wi-Fi 驱动内部诊断（排障时看）
(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\SWOA0D01\0\Device Parameters' `
    -Name SwitchWifiReport).SwitchWifiReport
```

`SwitchWifiReport` 里几个字段的含义：

| 字段 | 含义 | 正常表现 |
|---|---|---|
| `erx` | 收到的 EAPOL 帧数 | 每次连上都增长 |
| `kadd` | 已安装的密钥数 | 每次连上都增长 |
| `kfail` | 密钥失败次数 | **必须恒为 0** |
| `dn` | 断链事件数，末段是事件类型 | `E0` = 下次关联前的固件复位（正常） |
| `leave` | 断连阶段 0-4 | 正常断开时通常为 `0`（走事件路径） |

## 6. 期望的版本（作者机器实测，2026-11-18）

| 驱动 | 版本 | InfName |
|---|---|---|
| switchwifi | **0.7.4.0** | oem52.inf |
| switchjoycon | **0.3.20.0** | oem50.inf |
| switchbt | 1.0.0.19 | oem15.inf |
| switchfan | 1.0.0.22 | oem17.inf |
| switchaudio | 1.0.0.17 | oem12.inf |
| switchbattery | 1.0.0.11 | oem5.inf |
| switchpwrbtn | 1.0.0.4 | oem3.inf |
| switchrtc | 1.0.0.2 | oem9.inf |
| switchsio | 0.2.3.0 | oem11.inf |
| switchsensor | 1.0.0.1 | oem10.inf |
| switchtouch | 1.0.0.4 | oem2.inf |
| tegra210gpio | 1.0.0.4 | oem0.inf |
| tegra210i2c | 1.0.0.5 | oem1.inf |
| nvtsdhc（SD） | 0.4.0.1 | — |

`oemNN.inf` 的编号在不同机器上会不一样，**只有版本号和设备名有意义**。

---

## 7. 常见问题

| 现象 | 原因 / 处理 |
|---|---|
| `pnputil` 说驱动没签名 / 装了不生效 | 证书没导入，或 `testsigning` 没开且没重启 |
| 装完设备管理器显示 Code 10 | 同上；另外确认装的是**本仓库的包**（版本号必须 ≥ 已装的，`pnputil` 只升不降） |
| 装了 Wi-Fi 后 SSH 断了 | 正常：装 Wi-Fi 会重启网卡。本地操作不受影响；远程操作请用计划任务 |
| Wi-Fi 连上但不通网 | 看 `switchwifi` 的 `erx`/`kadd` 是否在长；`kfail` 是否 0；`SwitchWifiReport` 有完整字段 |
| 断连后必须禁用/启用网卡才能重连 | 那是 0.7.4 **之前**的缺陷，本包已是 0.7.4 |
| 触屏没反应 | `switchtouch` 是 `A9E1F054` 证书签的 —— 确认第 3 节那张证书导入了 |
| 风扇不转 / 背光不对 | 看 `switchfan` 是否装上；它的参数在 `HKLM\SYSTEM\CurrentControlSet\Services\switchfan\Parameters` |
| 手柄连不上 | `switchjoycon` 0.3.20 需要两条导轨都识别（左 UART-C / 右 UART-B） |
| 想退回旧 Wi-Fi 驱动 | 用 `packages\switchwifi-rollback-0.7.9.0-as-0.7.0.0\`（它是 0.7.0.0 的二进制，版本号标成 0.7.9.0 才能覆盖） |

## 8. 需要你自己编译的情况（只有这几种）

1. **想改 Wi-Fi / 其它驱动代码** → [BUILD.md](BUILD.md) §1-2
2. **想改 UEFI（WinX）** → [BUILD.md](BUILD.md) §3，需要 EDK2 工具链
3. 那 4 个 `A9E1F054` 签名的驱动，如果你**不想导入那张证书**，就只能自己重编重签 → [BUILD.md](BUILD.md) §2

除此之外，**装完即用，不需要任何编译**。
