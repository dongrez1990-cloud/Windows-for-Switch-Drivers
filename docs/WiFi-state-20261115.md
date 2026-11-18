# Switch 初代（Erista）WiFi —— 现状 · 正确路径 · 踩过的坑
2026-11-15 深夜整理。**新会话先读这一页**，不用再从头试。

---

## 一、现在卡上是什么（已经验收过的状态）

| 项 | 值 |
|---|---|
| 固件 | hekate 第 6 项 `43-swoa-v35`：`bl33` sha256 `E4B78C995382758C…`，FD 占用 0xd0eb8 |
| 固件行为 | BDS 先等 12 s（让 `UsbPowerAuditDxe` 的整机 bring-up 跑完）→ 加载 `\SwitchWOA\Wifi\WifiPrepare.efi` → patch `WSTA=0x0F` → **自己启动 `\EFI\Microsoft\Boot\bootmgfw.efi`**（不用 Shell、不用键盘） |
| WiFi 预处理 app | `E:\SwitchWOA\Wifi\WifiPrepare.efi`（53248 B，sha `2ECAC3CD…`，带屏幕日志 + 失败挂屏 25 s） |
| WiFi 驱动 | **switchwifi 0.7.0.0**，已装为 `oem13.inf`（2026-11-15 20:53 手动 `install-batch8.cmd` 确认 `已安装在设备上: ACPI\SWOA0D01\0`） |
| 风扇驱动 | 仍是 1.0.0.13（`oem14.inf`），`Duty=0` → 大概率停转；`install-batch9.cmd`(v2) 会先删包再装 1.0.0.8 |
| 其他驱动 | 117 音频 / bt13 / wifi072 等包都在 `C:\SwitchWOA\drivers\`，脚本在 `C:\SwitchWOA\setup\` |
| 开机 agent | `SwitchWOABringup` 服务 → `agent.cmd`：静默等 60 s → 跑 `agent.ps1` → 报告 + request.txt；**现在会额外在桌面写 `SwitchWOA-agent-status.txt`** |

---

## 二、正确的链条（每一步都别跳）

```
hekate 选 43-swoa-v35
  └ BDS: 等 12 s ──────────────► UsbPowerAuditDxe 定时器（R364 CPU / R366 风扇 / R373 触屏
                                 供电 / R379 传感器 / R390 蓝牙供电 / R393-R402 音频 / R363 USB-C）
  └ BDS: WifiPrepare.efi ──────► [0] 供电 LDO1+SD3（LDO7 只读）
                                 [bt] UART-D + BT_REG_ON（蓝牙先上电）
                                 [1] PexUp + PexEnum（PCIe 链路 + BAR0/BAR2，prep.opt 缺省=bars）
  └ BDS: 找到 DSDT 里唯一的 `08 'W''S''T''A' 0x0A 0x10` → patch 成 0x0F + 重算 checksum
  └ BDS: 自己 StartImage bootmgfw.efi（无键盘也能进 Windows）
  └ Windows: switchwifi 绑 ACPI\SWOA0D01 → 驱动自己开桥内存窗口（chip.c）→ 读 chip id / EROM
             → 下 brcmfmac4356A3-pcie.bin + .txt → WPA2 → DHCP
```

**驱动侧的行为契约**（旧会话验证过）：
- `Candidates()` 必须接受 **RoamRequest**（0.5.8.0 起已修）——否则"已保存网络/自动连接"全被拒（`join=25/C00000BB`）；
- 掉线后 **重建网卡 + 10 秒去抖**（0.7.0.0）→ 自动重连；
- **设备管理器里禁用/启用不会重做握手**，要重试只能冷启动。

---

## 三、踩过的坑（现象 → 真因 → 正确做法）

| # | 现象 | 真因 | 正确做法 |
|---|---|---|---|
| 1 | 每次开机 `[bds] wifi prep returned Not Found` | app 放在 ESP **根目录**，BDS 找的是 `\SwitchWOA\Wifi\WifiPrepare.efi` | 放到这个路径；`deploy-card.ps1` 已自动写两处 |
| 2 | 放对路径后仍 Not Found | v26~v32 全带 `SWITCH_ACPI_WIFI_ALWAYS_ON`，DSDT 里是 `WSTA=0x0F`，**marker 0x10 不存在** → 校验失败在 LoadImage 之前就返回 | 去掉该开关（v33+）；`WifiHandoff.c` 也改成"先跑 app，marker 唯一才 patch" |
| 3 | 进 Windows 后 `SWOA0D01 err=10`、`netsh` 无网卡 | 上一步没做 → PCIe/BAR 窗口从未准备 | 修好 1+2 后自然消失（v33 首次出现 `err=0`） |
| 4 | 卡在 `Shell>`，**键盘打不了字** | 初代机 UEFI 的 XUSB/PADCTL 没上电，没有 HID；而且 WiFi 预处理会重枚举块设备，Shell 的 `fsN:` 失效 → `startup.nsh` 落空 | 不要再依赖 Shell：BDS 直接 StartImage `bootmgfw.efi`（v34+） |
| 5 | 屏幕上不再出现 R364/R366/R404 那一串 | v34 在 `PlatformBootManagerAfterConsole` 里**立刻**启动 Windows，抢在 `UsbPowerAuditDxe` 的 10 s 定时器之前 → 整机 bring-up 全跳过 | v35 先 `gBS->Stall(12s)` 再 prep（已修，实测 R3xx 重新出现） |
| 6 | WiFi 起来后 ~15 s 蓝屏 / `INACCESSIBLE_BOOT_DEVICE` | 两种：① 第 5 条导致主板半准备；② **长按断电把 NTFS 弄脏** | 第 5 条已修；卷用 `chkdsk F: /f` 修（已做，复查 0 坏扇区）。**别长按电源** |
| 7 | 蓝屏但没有 dump 可分析 | **页面文件 0 MB**，Windows 写不出 minidump | 靠 System 日志：报告的 **10c 节**抓 1001/41/6008 并 `wevtutil epl` 导出 evtx；实测没有 1001（确实没 dump） |
| 8 | `wifiprep.log` 一直 0 字节 | 早期走 R408（DXE 阶段）时 FatPkg 写不出来 | app 在 **BDS 阶段**跑时写盘是好的（实测 2442 字节），所以现在日志可读 |
| 9 | 风扇"没风" | 1.0.0.9/1.0.0.13 在 29 ℃ 下 `Duty=0` | 1.0.0.8：最低 21%、开机即转、测温失败不停；**必须先用 `pnputil /delete-driver oem14.inf /uninstall /force` 删掉新包**，否则 pnputil 认为"已是最新"（batch9 v1 就是白跑） |
| 10 | 手动跑 CMD 弹出"类似防火墙"的窗口 | Windows UAC / SmartScreen | 选"允许/是"即可，脚本本身没联网 |
| 11 | `E:` 与 `F:` 分开"安全弹出"会报错 | hekate UMS 把一张卡导出成**同一个 USB 盘的两个卷** | 对 `E:` 弹一次即可，两个盘符一起消失 |
| 12 | 卡写几次后容易坏 | 强断 + FAT32 ESP 被写 | 每次拔卡前安全弹出；卡上 `EFI_backup\BCD.good` 是保命的 |
| 13 | 构建固件报 `error 0004: File read failure` | Windows python 用本地代码页读 UTF-8 的 DSC | 必须 `PYTHONUTF8=1`，并且用 `C:\devkitPro\msys2\usr\bin\bash.exe`（Git-Bash 没有 make） |
| 14 | `C:\Users\Switch\Desktop` / `Users\Public\Desktop` 在 PC 侧写不进去 | 离线 NTFS 的 ACL | 桌面快捷方式只能在 Switch 上用管理员跑 `place-desktop.ps1` 生成；PC 侧只往 `C:\SwitchWOA\...` 放东西 |

---

## 四、下一步（就一步）

1. 卡插回、选 **`43-swoa-v35`** 开机：
   - 应该看到 `R3xx` 一串 → `[bds] wifi prep ok: WFI0 published`（app 已恢复）→ `[bds] starting bootmgfw` → 进 Windows；
   - 进系统后 **`ACPI\SWOA0D01 err=0`** 且 `switchwifi Running`，`netsh wlan show interfaces` 应出现网卡。
2. 直接连家里 WiFi：第一次手动连；之后靠 0.7.0.0 的"断开即重建"自动重连。
3. 想确认状态：`C:\SwitchWOA\setup\wifi-status.cmd`（管理员）→ 生成 `C:\SwitchWOA-WiFi-status.txt`（设备码 / InfPath / `SwitchWifiReport` / netsh）。
4. 风扇没风：`C:\SwitchWOA\setup\install-batch9.cmd`（v2，管理员）。
5. 万一又崩：**别长按**，正常关机；卡回来我读 `F:\SwitchWOA\bringup\report-*.txt` 的 10c 节。

---

## 五、别再做的事

- ❌ 在设备管理器里"禁用→启用"网卡（不会重做握手）
- ❌ 黑屏/转圈时立刻长按电源（弄脏 NTFS，接着就是 0x7B）
- ❌ 给固件加 `SWITCH_ACPI_WIFI_ALWAYS_ON`（会破坏 WSTA marker）
- ❌ 把 `WifiPrepare.efi` 只放 ESP 根目录
- ❌ 指望 UEFI Shell 能手打命令（没有键盘）
---

## 六、2026-11-16 凌晨：SSH 打通 + 蓝牙第一次真正起得来

### 6.1 SSH（这是最大的效率提升）

Switch 上装了 OpenSSH（桌面 `11 SSH-安装(管理员).cmd`），`woa` 账号**是管理员**：

```
ssh -i D:\paopao\ssh\woa_ed25519 -o UserKnownHostsFile=NUL woa@192.168.1.74
```

远端默认 shell 是 **PowerShell**（不是 cmd，`&&` 不可用）。传脚本最稳的方式是走 stdin：

```powershell
$script | & ssh -i D:\paopao\ssh\woa_ed25519 -o UserKnownHostsFile=NUL woa@192.168.1.74 powershell -NoProfile -Command -
```

### 6.2 蓝牙：根因是 `SafeMode=1` 卡住，清掉就通

| 时刻 | switchbt Parameters |
|---|---|
| 之前（每次开机） | `Stage=3 SafeMode=1 BootPending=1 Tx=0 Rx=0 EvIn=0`（驱动被安全模式挡住，一个字节都没发） |
| 清掉 `SafeMode=0`/`BootPending=0` 后 10 s | `Stage=5`，随后 `Stage=6`、`Result=ok` |
| 最终 | `Stage=6 SafeMode=0 Result=ok LocalName=BCM4356A3 BdAddr=98:B6:E9:9E:39:27 Manufacturer=0x000F UartClockMHz=408 HighSpeedResult=2`（3 Mbaud + 流控）`FirmwareResult=0`（ROM 码，**不需要 BCM4356A3.hcd**） |

设备树同时长出来了：
- `SWOA\BTH_UART_H4\0` → **Bluetooth Radio**，status=OK problem=0 svc=BthMini
- `BTH\MS_BTHLE` / `BTH\MS_BTHBRB` / `BTH\MS_RFCOMM` 全部 OK
- `bthserv` / `BthAvctpSvc` / `BTAGService` 全部 Running

也就是说：**芯片、UART、3 Mbaud 切换、固件探测、BthMini 栈全部正常**，卡点纯粹是安全模式的粘滞状态。

等价的官方操作就是 kit 的 `Bluetooth.ps1 -Mode Retry`（`kits/Switch-Bluetooth/`），它会清 `BootPending` 让驱动重试。

### 6.3 然后是坏消息：之后 Switch 从网络上消失了

蓝牙刚起来约一分钟，`ping 192.168.1.74` 失败、SSH 连续两次 `Connection timed out during banner exchange`。
`arp -a` 里 `192.168.1.74 → 98-b6-e9-58-20-09` 还留着（WiFi MAC），所以不是"换 IP"。

结合文档记录 **"1.0.0.13 曾经启动挂死"**，最可能是 switchbt 1.0.0.13 在 BthMini 开始收发后把系统挂住了。

**下一步**：换 **1.0.0.19**（已备好 `bringup\staged\switchbt19` + `install-batch10.cmd`）。它相对 1.0.0.13 的关键修复正是这一类：

- 1.0.0.14：只在 UART-D 中断已连接时派发 HCI；D0 退出前排干定时器
- 1.0.0.15：允许 BthMini 在父设备 D0 退出前取消在途写，并保留已拷贝的 H4 帧直到 ISR 完成
- 1.0.0.17：下载前校验整包、逐条校验命令状态、校验波特率
- 1.0.0.18：`BootPending` 只记录未完成的探测，不再在后续开机禁用蓝牙
- 1.0.0.19：D0 上下文丢失后恢复 UART-D 的 CAR enable/reset，要求 PLLP 父时钟
---

## 七、2026-11-16：Tailscale + Moonlight（远程通道打通）

| 节点 | Tailscale 地址 | 备注 |
|---|---|---|
| 这台 PC（`dongrez`） | **100.69.65.12** | Sunshine 已装并在跑（服务 + 进程，监听 47984/47989/47990/48010） |
| **Switch（`switch\woa`）** | **100.106.117.117** | OpenSSH 22 端口，`woa` 是管理员；`ssh -i D:\paopao\ssh\woa_ed25519 woa@100.106.117.117` |
| 旧节点 `switch-1` | 100.92.200.52 | tailnet 里的历史记录（2 天前离线），建议在管理后台删掉避免混淆 |

局域网地址：Switch `192.168.1.74`，PC `192.168.1.9`。

### Moonlight（Switch 端串流客户端）

- 版本：**Moonlight 6.2.0 ARM64 原生**（`MoonlightPortable-arm64-6.2.0.zip`，32.7 MB）
- 位置：`C:\SwitchWOA\apps\Moonlight\Moonlight.exe`（Qt6 + ffmpeg 全套 arm64 dll）
- 连接 PC：Add PC → `100.69.65.12`（走 Tailscale，外网可用）或 `192.168.1.9`（局域网）
- 配对：Moonlight 显示 4 位 PIN → PC 上打开 `https://localhost:47990`（Sunshine Web UI）→ 输入 PIN
- **注意**：Switch 上没有 NVIDIA 驱动，**NVDEC 硬解不可用**，只能软解；建议先 720p60 / 1080p30、码率 10–15 Mbps 试，再往上加。

### SSH 掉线的观察

2026-11-16 凌晨出现过：能 TCP 连 22 端口（`Test-NetConnection -Port 22` = True）但 **SSH banner 超时**，
随后两个地址都连不上；与此同时 `arp -a` 里 `192.168.1.74 → 98-b6-e9-58-20-09` 仍在。判断是这块 WiFi/驱动
在高负载下（Clash 安装、Moonlight 解压、磁盘 100%）握手被拖死，不是 sshd 配置问题。
处理办法：等负载过去，或本地重启一次 `Restart-Service sshd`。
---

## 八、2026-11-16：v36 之后"掉 UEFI Shell"的真因（已修：v42）

### 症状链（很误导人）

- v36 起每次开机都停在 `Shell>`，Windows 进不去；
- v35 之前是能进的，但我一改就坏 → 一度怀疑是自己清理 ESP 弄坏了卡；
- v40 的卷探测给出决定性证据（屏幕原文）：

```
[bds] vol 0: bootmgfw=yes wifiprep=yes bootaa64=yes     ← 文件都能打开！
[bds] loading \SwitchWOA\Wifi\WifiPrepare.efi ...
[bds] wifi prep returned Not Found                      ← 紧接着就 Not Found
[bds] bootmgfw returned Not Found
```

即"**同一卷、同一路径，先能开、几毫秒后再开就 Not Found**"。同时卡上 `bds.log` 被创建但**写入 0 字节**（和 `wifiprep.log` 曾经 0 字节、`switchwoa-uefi.log` 写不出来同源）。

### 真因

v35 的 BDS 是 **盲等 12 秒**再 prep；v36 改成"**轮询 bring-up 完成协议，一旦出现立刻继续**"。于是 prep/启动在 `UsbPowerAuditDxe` 的整机 bring-up 结束后的 **0–100 ms** 内就去读 SD 卡 ——
**SD 控制器/卡还没缓过来，读就失败**（这块卡在高速连续操作后需要一点静默时间）。

### 修法（v42）

在轮询成功后加 `gBS->Stall (3 s)` 再碰文件系统；同时把固件侧所有 FAT **写**操作全部关掉
（`BdsLog()` 直接 return、`BdsProbeVolumes()` 不再调用、`R404OpenResultLog()` 直接返回失败）。

**v42 上机结果：进 Windows ✅**（`00-swoa-v42`，`bl33` sha `BD3DF9CF583AF4C7…`）

### 教训（下次别再踩）

1. **初代机 UEFI 读 SD 有"冷静期"**：bring-up / 大量 IO 之后立刻读 ESP 会 Not Found，等 2–3 秒就好；
2. **固件侧不要写 FAT**：这块卡上 create 能成功但 write 出 0 字节，而且会连累后续读；
3. 诊断一定要**上屏**（Shell 一出现 console 就被冲掉，写文件又写不进去）；
4. 变体矩阵很有用：v42（轮询+settle）与 v43（盲等 12 s）同时放在菜单里，一次开机就能二选一。
---

## 九、风扇曲线（自编驱动，1.0.0.21 → 1.0.0.22）

固件出厂曲线（1.0.0.7/13 那份）在 Erista 上是 `≤39°C → Duty 0`、`42°C → 51/236(22%)`，
所以低负载时风扇基本不转（"没风"）；而 1.0.0.13 更是全程 `Duty=0`。

自编版本（源码 `drivers\SwitchFan\src\switchfan.c`，用 `tools\Build-Driver.ps1` 编译并签名，
证书 `CN=SwitchWOA Test Driver Signing`，Switch 已信任）：

| 版本 | 策略 | 实测 |
|---|---|---|
| 1.0.0.21 | 无条件保底 45%，曲线 40→50%,43→55%,47→64%,52→74%,58→85%,65→100% | 31°C 时 Duty=106（45%），"风够了" |
| **1.0.0.22** | **亮屏保底 25%（59/236）；熄屏且 <45°C 停转，≥45°C 强制按曲线转** | 21°C 亮屏 → Duty=59（25%），oem17.inf |

产物：`D:\paopao\build\fan121\out`、`D:\paopao\build\fan122\out`；
卡上：`C:\SwitchWOA\drivers\switchfan121\`、`switchfan122\`。
安装：`pnputil /add-driver ... /install /force`（版本号更高会直接顶替，不必先删包）。

注意：`Parameters\DriverVersion` 是源码里的常量，改 INF 的 DriverVer 不会改它（仍显示 7），
**判断实际装的是哪版要看类键 `Control\Class\{4d36e97d-…}\xxxx\DriverVersion` 与 `InfPath`**。
