# 恢复包 2026-11-18 —— 从 GitHub 重建这台 Switch 的驱动状态

> 用途：**本地机器彻底丢了也不怕**。这里 + GitHub 上的两条分支，足以重建出 2026-11-18 这天
> 机器上跑的那套驱动（switchwifi 0.7.4.0 等），并重新装回 Switch。

---

## 一、这份包里有什么

| 目录 | 内容 | 为什么要单独带 |
|---|---|---|
| `sources\` | `switchjoycon`（手柄驱动）、`SwitchPad`（安卓上位机）、`SwitchKey` | **这三个项目不在驱动仓库里**，GitHub 上原本没有 |
| `packages\` | 机器上**实际安装**的成品驱动包（`.sys/.inf/.cat`，已签名），每个驱动一份 | 不重新编译也能直接装回去 |
| `docs\` | 交接文档、0.7.3/0.7.4 报告、0.7.2 基线清单、上机日志、故障记录 | 出问题时对照症状 |
| `tools\` | `Build-Driver.ps1`、`Build-SwitchWifi.ps1`、`Test-SwitchWifi.ps1`、`HostTests\`、`Patch-NdisHeader.ps1`、`wdk-patch\`、测试证书、`pack-switchwifi-rollback.ps1`、`deploy-scripts\`（上机用 SYSTEM 脚本） | 重建与上机所需的全部脚本 |

## 二、GitHub 上对应的两个位置

| 位置 | 内容 |
|---|---|
| 仓库 `ruje0504-2/Windows-for-Switch-Drivers`<br>分支 **`erista-0.7.x`**（tag `erista-0.7.4.0`） | 改过的驱动**源码**：SwitchWifi 0.7.2→0.7.4、SwitchAudio、SwitchPowerButton、Tegra210Sdhc |
| 同一仓库<br>分支 **`restore/20261118`** | **本恢复包**（这些文件本身） |

> `erista-0.7.x` 是**留档支线**：它基于 `e220195`（11-08），**落后默认分支 30 个提交**，
> 且相对默认分支会删除 SwitchFan / SwitchBattery 等上游新代码。**永远不要把这条支线合并进默认分支**，
> 它只用于"取回我们那版源码"。

## 三、重建环境（构建机）

| 需求 | 说明 |
|---|---|
| Windows 10/11 **x64** | 交叉编译 ARM64 驱动，不需要 ARM 机器 |
| **Windows Kits 10.0.26100.0** | WiFi 驱动必须用这个版本；22621 的 WiFiCx 头文件编不过 |
| **LLVM**（`clang-cl.exe` + `lld-link.exe`）在 `C:\Program Files\LLVM\bin` | 构建与主机测试都用它 |
| Windows PowerShell 5.1 | 本机没有 PowerShell 7，脚本都用 `powershell -File` 跑 |
| ~~Visual Studio / MSVC CRT~~ | **不需要**：主机测试用 freestanding shim（`tools\HostTests`）编译 |
| 目标机 | 开启测试签名模式 + 信任 `tools\cert\SwitchWOA-Test.cer` |

## 四、从零重建（照抄即可）

```powershell
# 1. 取源码
git clone https://github.com/ruje0504-2/Windows-for-Switch-Drivers.git
cd Windows-for-Switch-Drivers
git checkout erista-0.7.x

# 2. 跑主机测试（不需要上机，14 个用例）
powershell -File tools\Test-SwitchWifi.ps1
#   期望最后一行：--- 14 test(s): 14 passed, 0 failed ---

# 3. 编 WiFi 驱动（0.7.4.0）
powershell -File tools\Build-SwitchWifi.ps1 -OutDir build\SwitchWifi074
#   产物：build\SwitchWifi074\{switchwifi.sys, .inf, .cat}
#   期望 switchwifi.sys = 179112 字节，刷新鲜签名后 SHA256 会变（签名带时间戳）
#   0.7.2 的参考哈希：56F77108CA5B860B655...  （见 docs\MANIFEST-wifi-0.7.2-sha256.txt）
```

> 不想编译也行：`packages\switchwifi-0.7.4.0\` 里就是当天跑通的成品，直接跳到第五节装回去。

## 五、装回 Switch（铁律都在这里）

```powershell
# 1. 拷到机器（保持一条目录一层版本）
scp -i <你的密钥> packages\switchwifi-0.7.4.0\* woa@<Switch-IP>:C:/SwitchWOA/drivers/switchwifi-074/

# 2. 安装。注意：装驱动会重启网卡，而 WiFi 就是你的 SSH 通道 —— 所以用 SYSTEM 计划任务执行
#    （现成脚本见 tools\deploy-scripts\install-074.ps1）
pnputil /add-driver C:\SwitchWOA\drivers\switchwifi-074\switchwifi.inf /install

# 3. 验证
(Get-PnpDevice -InstanceId 'ACPI\SWOA0D01\0').Status                      # OK
(Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0D01\0' `
    -KeyName DEVPKEY_Device_DriverVersion).Data                            # 0.7.4.0
Get-NetAdapter -Name WLAN | Select Status,LinkSpeed                         # Up / 866.7 Mbps
```

**铁律（违反过、代价很高）**

1. `pnputil` **只升不降**。要回退，必须"同一个二进制 + 更高 DriverVer + 重签 cat"
   → 用 `tools\pack-switchwifi-rollback.ps1`（`packages\switchwifi-rollback-0.7.9.0-as-0.7.0.0\` 就是现成回退包，
   它装的是 **0.7.0.0 的原始二进制**，只是版本号标成 0.7.9.0）。
2. **绝不**在设备管理器里"卸载设备 + 扫描硬件改动"——会卡死 PnP/服务线程。
3. 断连测试/装驱动会掐断 SSH。一切"会掉网"的操作都走 **SYSTEM 计划任务 + 写日志**，事后回读。
4. `scp` 的 sftp 子系统会挂（`banner exchange` 超时），但 `ssh` 能通 —— 取日志用
   `ssh ... "powershell -NoProfile -EncodedCommand <base64>"`。
5. 强制关机代价极高（脏位→索引损坏→配置丢失→蓝屏）。能等就等计划任务自愈。

## 六、当天机器上的驱动版本

| 驱动 | 版本 | 备注 |
|---|---|---|
| **switchwifi** | **0.7.4.0**（oem52.inf） | 本次成果：5/5 + 10/10 断连重连通过，866.7 Mbps |
| switchfan | 1.0.0.22 | |
| switchbt | 1.0.0.19 | |
| switchaudio | 1.0.0.17 | |
| switchjoycon | 0.1.1.0 | 源码在 `sources\switchjoycon\` |
| switchsio | 0.2.3.0 | |
| switchsensor | 1.0.0.1 | |
| Tegra210Sdhc | 0.4.0.1 | |

（精确清单见 `docs\INSTALLED-20261118.txt`；若该文件为空，说明抓取时 SSH 正在抽风，重跑即可。）

## 七、这次 WiFi 到底改了什么（一句话版）

- **0.7.3.0**：把 GitHub 上游 Lite 0.5.7 的断连策略搬进来（`ConfirmLinkDown` 回读 BSSID 后才决定是否判死 + `leave=phase/status` 上报）。
- **0.7.4.0**：修 `RxComplete()` 的 epoch 判定——重连后 EAPOL M1 落在旧的空缓冲区上会被当过期包丢掉，导致 4 次握手超时、必须禁用/启用网卡。改为按**到达时刻**记 epoch。
- 细节与证据链见 `docs\WIFI-073-074-REPORT.md`。
- **注意**：这两处上游（`2d82f24`）其实都已经有了，我们这棵树是从 11-08 的旧快照分出来的，属于"重新推导了一遍"。
  所以这条支线是**留档**，不是可合并的 PR。
