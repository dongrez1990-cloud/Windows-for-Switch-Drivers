# GPU0 (T210/Erista) 测试交接 —— 2026-11-21 更新（原 2026-11-20）

> 给下一个会话的接续说明。历史全过程在 `STATUS-GPU0-BOOT.md`（第 9 章是早期、第 10 章是 11-19/11-20 这两天的实测，
> **第 16 章是 11-21 下午的 P0-1 提频轮**），本文只写**现在是什么状态、什么已被证实/推翻、下次从哪继续**。

---

## 1. 一句话现状

**2026-11-21：P0-1 完成 —— GPU 零售 OPP 阶梯四档（460.8 / 691.2 / 768 / 844.8 MHz）真机全部跑通。**
`HardwareStatus=0`、`ClockResult=0`、`ExecutionResult=0`、`GrStage=9`（`0x600dc0de`）、`StatusWorker=0`，
每档 `BootGpuMeasuredKhz` **精确等于**目标频率，电压由 CVB 自动给出（speedo=2031，844.8 MHz → 1,106,250 µV）。
档位现在**只靠注册表 `Parameters\BootGpuKhz` 切换**，不用重编驱动。921.6 MHz 被 CVB 的 `max_mv=1113`
挡住（要求 1,162,500 µV），需要 DTS/电压域改造才能上。

**2026-11-20 23:39 里程碑：Erista GPU 的内核侧 bring-up 全线打通。**
`HardwareStatus=0`（DdiStartDevice 成功）、设备管理器 `Tegra X1 (GM20B) render adapter (experimental)`
status=OK / problem=`CM_PROB_NONE`（Code 0）、`BootGpuMeasuredKhz=0x70800`（460.8 MHz 实测）、
`ExecutionResult=0`（MMU/PFIFO/PMU/ACR/GR 固件全通过）、`GrStage=9`（golden context `0x600dc0de`）、worker 已接管。

链路：ACPI/资源 → 电轨（GPIO6 + MAX77621）→ 释放轨门（`PMC+0x2D4=0`，须电轨稳定后）→
**priv ring 初始化（`0x12004C=4` 等，这是 GPCPLL/GPC 页的总开关）** → GPCPLL（460.8 MHz）→
MMU/PFIFO/PMU/ACR/GR 固件 → USERD → golden context → 图形模板 → worker。

**这不等于"GPU 能显示"**：`tegragpuno3d.dll` 仍是 2 KB 占位，DWM 走软件渲染。
下一阶段是 B 阶段：KMD 提交/同步契约 → UMD(DDI) → DWM 合成与共享资源 → DXVK。

> 历史（已作废的结论）保留在 STATUS 第 9–12 章，供追溯：曾误判为"TD 分区只能由 BPMP 打开"、
> "PWRGATE_STATUS bit1 是判据"、"PLL 页不可访问是轨门未释放"。真正的根因与修正见第 13–15 章。

---

## 2. 现场状态（卡上/仓库）

| 项 | 内容 |
|---|---|
| 卡上固件 | `E:\switchroot\winx-gpu0\bl33.bin` = **v72**（`F766B207E89489F5…`），DSDT：GPU0 11 窗口 + FAN0 三条 I2C(0x4C/0x1C/0x3C) |
| 启动项 | `06-swoa-gpu0.ini`（GPU0 测试项）用这个目录；`00-swoa-v46.ini`（`switchroot/winx-v46`）**未动**，可随时回退 |
| 回滚镜像 | `E:\switchroot\winx-gpu0\bl33-v5x/v6x/v67/v68-held.bin` 一系列 |
| KMD 包 | `F:\SwitchWOA\gpu-pkg\` = **0.1.0.112**（sys SHA256 `0C78DA6A…`）= `BootGpuKhz` 档位 + CVB 上限守卫 + **`SelfCheck` 汇总记录 + 工作档开关默认值**；0.1.0.111 `26846120…`、0.1.0.110 = 四档全绿版、0.1.0.109 = 更早全绿版。源码 `D:\paopao\erista-gpu\src\mesa-switch-woa\tegra-x1-wddm` |
| switchfan 包 | `F:\SwitchWOA\fan-pkg\` = **1.0.0.31**（含 MAX77620 0x3C 通路）；源码 `D:\paopao\WoA\erista-gpu\switchfan-t210` |
| 构建目标 | `WoA\build\build-gpu0.sh` 里 `V=v72`；KMD 包版本在 `kmd-package\tegragpurender.inf` 的 `DriverVer`（用 `tools\package-kmd.ps1` 改） |
| **当前开关** | **注册表里已没有任何人工开关**（2026-11-21 20:4x 起驱动缺省值即工作档：`Limit=5`、`Mask=0`、`RgWrite=1`、`ClampNeedsTd=0`、`PriRing=1`、`RailSettleMs=25`、`BootGpuKhz=460800`）。真机验证：全空启动 → `SelfCheck=1`、`HardwareStatus=0`、`GrStage=9`、`measured=460800`。要改档位用 `.tmp\set-boot-khz.ps1 -Khz …`，保守档用 `install-safe.cmd` |
| 只上电档（省一半风险） | `HardwareStartLimit=1`、其余同上：只到上电+孔径探针可控 |
| 安全档（随时可回） | 卡上双击 `F:\SwitchWOA\install-safe.cmd`（`Limit=1` + 全部探针关） |

**下次开机前**：若不确定卡上开关，先双击 `F:\SwitchWOA\install-safe.cmd`（会重写上面这套安全档）。

---

## 3. 取证链（这两天最有价值的基建）

1. **驱动侧**：`t210_power.c` 的 `record32()` 用 `ZwOpenKey + ZwSetValueKey + ZwFlushKey` 写注册表，
   并且 `record_step()` 每一步把**全部记录写穿**到 `C:\SwitchWOA\gpu-run.txt`（`FILE_WRITE_THROUGH`）。
   → 即使当场挂死/长按断电，也能拿到"最后一步"。
   → 读法：**关机 + UMS**，直接读 `F:\SwitchWOA\gpu-run.txt`，**不需要 SSH、不需要管理员**。
2. **步骤号**：1–14 主流程；20/21 CAR 时钟；23/24 清钳位；30–32 电轨扫描；40–44 孔径探针；50/51 关机恢复。
3. **开关**（注册表 `HKLM\SYSTEM\CurrentControlSet\Services\tegragpurender\Parameters`）：
   `RequireTdStatus`、`RgWrite`(0/1/2/3)、`RailSweep`、`ClampNeedsTd`、`ApertureProbe`、`RailProbe`、`ClockFirst`、`PowerSkipMask`(bit0 电轨/bit1 时钟/bit2 复位/bit3 MC 排空/bit4 MC_FLUSH/bit5 分配/bit6 PMC 门控/bit7 前端)。
4. **UEFI 侧取证不可靠**：`PmicDxe` 的打印会被 `\SwitchWOA\Wifi\WifiPrepare.efi`（wifiprep 会 hold 屏幕 6 s）覆盖；
   ReadyToBoot 写文件也没产出；PMC scratch 被固件占用（`UefiMagic` 永远读不到）。

---

## 4. 已证实的事实（真实硬件，可复现）

| 事实 | 关键证据 |
|---|---|
| ACPI 枚举、资源契约 OK | `ResourceStatus=0`，设备 Code 0；11 内存窗口 + LIC + 2 个中断必须**精确**匹配 `t210_resources.h` |
| **GPU 零售 OPP 阶梯四档全通** | 460800 / 691200 / 768000 / 844800：`BootGpuMeasuredKhz` = 目标（误差 0），CVB 电压 868,750 / 1,000,000 / 1,050,000 / 1,106,250 µV，`ClockResult=0`、`ExecutionResult=0`、`GrStage=9` 全绿（STATUS 第 16 章） |
| **CVB 模型对 speedo=2031 精确成立** | `ClkNote6`=2031（`0x7EF`）；`t210_opp.c` 公式算出的四档电压与下发值**逐位一致**，`ext_cal` 23→36→40→46 全在 ±64 内 |
| **921.6 MHz 上不去（电压域限制）** | CVB 要求 1,162,500 µV > `max_mv` 1,113,000；0.1.0.111 起该档直接 `RAIL_ERROR`，不再回退到在飞电压强开 |
| KMD 上电流程全通（除尺寸/电压外不碰孔径） | `HardwareStage=105`、`PowerResult=0` |
| **Windows 侧 PMC toggle 通路有效** | VIC(23) 两次 toggle：`0x3FFECF4D → 0x3F7ECF4D → 0x3FFECF4D` |
| **TD(1) 的 toggle 被接受但状态不变** | `TdTglAck=0`（START 自清）但 `TdStatus1/1b/2/2b/Final` 恒 `0x3FFECF4D` |
| 电轨电压不是原因 | 900/1000/1100 mV 三档重试 toggle，TD 仍 0 |
| 清 GPU 钳位（`0x7000E6D4` 写 0 或清 bit0）**必挂总线** | `Step 6`/`Step 23` 之后无后续记录 + DPC_WATCHDOG；写回同值 1 则正常（`Step 8`） |
| **CAR 用 X 组是对的**（不是 Y 组） | U-Boot 派生的 T210 时钟驱动里 `/* 184 */ NONE(GPU)` 属 X 段；实测 `ClkXBeforeSet=0x23020780`（bit24/29 已置） |
| TD 不上电时读孔径必挂 | `ApertureProbe` 每次卡在 `Step 40`（第一次读 `0x57000000`）→ WHEA |
| MAX77620 看不到轨状态 | `GPIO6=0x09`（输出高）后，`PwrDetVal` 与 `In0/In1/In7` 在拉低/拉高四个时点**完全不变** |
| `PG_MASK/SEC_DISABLE` 不是屏蔽原因 | `PmcPgMask=0xFFFFFFFF`（与 Linux 一致）、`PmcSecDisable=0` |
| 板型是 Erista/T210 | `BootHidrev=0x00022117` → chipid=0x21、major=1、minor=2（B01 会报 0x212x 且用 MAX77812@0x33） |
| `CLAMP_STATUS(PMC+0x2C)` bit1 恒 0 | 即 TD 未被钳位；`GPU_RG_CNTRL(0x2D4)` = 1 |
| T210 的 power-domain 表无分区 1 | `dt-bindings/soc/tegra210-powergate.h` 只有 `3D1 = 13`；13 是否可上电待本轮 `ToggleProbe` 验证 |
| Mariko 不需要开关分区 | `platform/t214_power.c` 只清 `0x2D4` bit0，从不写 `PWRGATE_TOGGLE` |

已推翻的旧结论：
- ❌"UEFI/Windows 直写 PMC 无效" —— VIC 能搬动，toggle 通路有效；
- ❌"TD 只能由 BPMP 打开" —— L4T 内核里**根本没有 BPMP**（`git grep mrq_pg` 为空）；
- ❌"`0x7000E6D4` 是 T214 偏移、T210 应该用 PMC+0x2C/0x34" —— `has_gpu_clamps=true` 的 T210 就是用它；
- ❌"GPU 时钟/复位用错了 bank（Y 组）" —— 实测 X 组才对。

---

## 5. 真正的根因（已确认）与已推翻的假设

**根因只有三条，按发现顺序**：

1. **释放 GPU 轨门（`PMC+0x2D4=0`）必须发生在 VDD_GPU 真的上电之后**。
   之前所有尝试都在 MAX77620 GPIO6 仍为输入态（电轨关）时写，必挂总线 → WHEA / DPC_WATCHDOG。
2. **priv ring 未初始化是 GPCPLL/GPC 页拒答（返回 `0xBADF3000/0xBADF5000`）的唯一原因**。
   `PowerSkipMask=0x80` 一直跳过前端启动，而 priv ring 初始化就在那个函数里。
   正确的三步（nvgpu `gm20b_priv_ring_enable`）：`0x1200A8=0` → `0x12004C=4` → `0x122204=2`。
3. **执行阶段的 FRONTEND 失败是 bus-interrupt 位残留**：`0x1100/0x1140` 需要写 ack（在前端启动里），
   否则 `InvBusIntr=0xC` 被当成 `PRI_FAULT`，执行阶段报 `EXEC_FRONTEND`。

**已推翻（不要再走）**：
- ❌"TD 分区只能由 BPMP 打开"（Erista 无 BPMP；L4T 也是 CPU 直写）；
- ❌"`PWRGATE_STATUS` bit1 是 GPU 是否上电的判据" —— 该位在 GPU 完全可用时仍为 0；
- ❌"`0x7000E6D4` 是 T214 偏移" —— `has_gpu_clamps` 的 T210 就是用它；
- ❌"GPCPLL 与孔径形成死循环" —— priv ring 一开，PLL 页立即可读：
  `CFG=0`、`COEFF=0x14201`、写读回 `0x12345/0x54321` 全部正常；
- ❌"UEFI 侧 PmicDxe 的 GPU 上电无效" —— 实测 KMD 启动时 GPIO6 又回到输入态（`Gpio6Before=0x02`），
  说明 UEFI 那次拉高没有留下效果，不能据此判断 UEFI 通路能否成功。

---

## 6. 不要再做的事（会浪费一轮开机）

- 不要再 toggle 分区 13（3D1）：会直接挂总线；
- 不要再把孔径探针（`ApertureProbe`）与"轨门未释放"组合起来试：`RgWrite=0` 时孔径必挂；
- 不要相信 `gpu-run.txt` 的"最后一步 = 崩溃点"，先确认该轮的 `record_step` 数量是否覆盖到那个阶段
  （文件上限已提到 192，末端 `record_step(60)` 表示整段序列返回）；
- 不需要再去 UEFI 里尝试上电（B 路线）或装 L4T 做对照（C 路线）：问题已在内核侧解决。

---

## 7. 工具与构建速查

| 目的 | 命令/脚本 |
|---|---|
| 编固件（bl33） | `WoA\build\build-gpu0.sh`（msys2 bash，`V=vXX` 控制版本）→ `WoA\rebuild\out\bl33-swoa-vXX.bin` |
| 编 KMD | `powershell -File WoA\erista-gpu\tools\build-kmd-t210.ps1` → `kmd-build\tegragpurender.sys` |
| 打包+签名 KMD | **`powershell -File WoA\erista-gpu\tools\package-kmd.ps1 -Version 0.1.0.xxx`**（一步做完：换 sys → 改 INF `DriverVer` → **先签 sys** → `Inf2Cat /os:10_RS5_ARM64` → **再签 cat** → 打印 SHA256。顺序反了 cat 里哈希对不上。证书 `CN=SwitchWOA Test Driver Signing`，A7C0A920…；本机显示 UntrustedRoot 属正常，测试根证书只在 Switch 上受信） |
| 改 GPU 档位 + 重启 | `powershell -File D:\paopao\.tmp\set-boot-khz.ps1 -Khz 844800`（顺带重压工作档开关；`-NoReboot` 只写不重启；回退档位 = `-Khz 460800`） |
| 编 switchfan | `powershell -File WoA\erista-gpu\tools\build-switchfan-t210.ps1`（产出 1.0.0.31 并签名） |
| 板级单测 | `WoA\erista-gpu\tools\build-board.ps1`（应 28/28 通过） |
| **PC 侧主机测试（新）** | `powershell -File WoA\erista-gpu\tools\run-host-tests.ps1`：把 `t210_clock.c` + `t210_opp.c` 原样编成 x64 程序跑，覆盖频率阶梯/电压/CVB 上限守卫，**不用重启机器**（本机无 MSVC vcruntime，故 `/nodefaultlib` + 只链 Kernel32） |
| **上屏链路 CPU 基准（新，免驱动）** | `powershell -File WoA\erista-gpu\tools\build-screenbench.ps1` → `tests\arm64-bin\screenbench.exe`；scp 到卡上直接跑（无需签名）：1/4 线程的帧拷贝、90° 旋转（朴素/分块/指针递推）、缩放、融合缩放旋转、SrcOver 混合，以及每帧总预算 |
| **CPU 主频探针（新，免驱动）** | 同一条命令编 `tests\cpuprobe.c` → `cpuprobe.exe`；用 `QueryThreadCycleTime` + `Sleep()` 反证测实际核心频率（当前机器 ≈1750 MHz），卡上 `C:\SwitchWOA\cpuprobe.exe` |
| 离线装整套（换 sys + 设开关） | 卡上 `install-stack.cmd` → `offline-install-stack.ps1`（参数见文首） |
| **在线装机（SSH，推荐）** | `powershell -File WoA\erista-gpu\tools\push-kmd-live.ps1 -Version 0.1.0.xxx -Limit 5 -Mask 0x0 -RgWrite 1 -ClampNeedsTd 0 [-BootGpuKhz 460800]`<br>→ scp 包 → `pnputil /add-driver` 暂存 → 类键指向新 oemNN.inf → `ConfigFlags|=0x20`（下次开机重装）→ 写开关 → 重启。**不热装、不重启设备**。控制台 SSH 账号 `woa` 已提权。 |
| 等重启后自动取数据 | `powershell -File D:\paopao\.tmp\pull-run.ps1 -Out <文件> -ExpectKhz <档位>`：用 `ClkNote3`/`ClkNote7` 判定"这次开机跑到了期望档位"（旧 `pull-key.ps1` 按字符串 key 判定，仍可用） |
| 抗丢包的远程命令 | `powershell -File D:\paopao\.tmp\rsh-retry.ps1 -Out <文件> -Needles <必须出现的串>`：远端命令写在 `D:\paopao\.tmp\remote.txt`，避免多层 shell 引号地狱 |
| 安全档 | 卡上 `install-safe.cmd` |
| 只看驱动记录 | 关机 + UMS → `F:\SwitchWOA\gpu-run.txt`（现在上限 192 条，`record_step(60)` 为"序列跑完"标记） |
| 读寄存器快照（需管理员） | 卡上 `inspect-gpu.cmd` → `F:\SwitchWOA\gpu-report.txt`（会加载离线 SYSTEM hive） |

---

## 8. 环境坑（会浪费时间的，务必记住）

0. **SSH 会话是 session 0，不能用来测屏幕/合成**（2026-11-21 教训，见 STATUS 第 23 章）：session 0 有一块
   合成的 1024×768 虚拟显示、没有 DWM。屏幕相关的读数（GetDeviceCaps/GetSystemMetrics/DwmFlush/GDI 到屏）
   **必须用 `-Principal <登录用户> -LogonType Interactive` 的计划任务在 session 1 里跑**（脚本
   `C:\SwitchWOA\session-probe.ps1`）。真实值：桌面 1280×720、信号 720×1280、合成开启、
   `DwmFlush` 稳定 21.78 ms/帧（≈45.9 Hz，且 DWM/System CPU 0% → 是**扫描率天花板**，不是软件旋转）。
1. **SSH 连不上多数是 Wi-Fi 链路丢包，不是 sshd 状态**（2026-11-21 更正）：现象是"TCP 能连、banner 超时"
   甚至"认证成功但远端 shell 不返回任何输出"，而控制台**本地**探针一直正常；同一分钟内重试 6 次，第 6 次成功。
   处理办法是**客户端重试**（`rsh-retry.ps1`，`ConnectTimeout=30`，20 秒一轮），不要频繁 `net stop/start sshd`。
   卡上另留了自愈任务 `SwitchWOA-SshdKeeper`（开机 + 每 2 分钟跑 `C:\SwitchWOA\sshd-keeper.ps1`，
   **只在本地探针失败时**才重启 sshd，健康时不打断会话；日志 `C:\SwitchWOA\sshd-keeper.log`）。
   顺带：这台机器 **ping 从来不通**（ICMP 被挡），别用 ping 判存活，用 SSH。
2. **驱动版本切换不会自动生效**：Windows 对已绑定设备不会因"存储里有更新版本"而切换；
   可用两条路：(a) 改类键 `Control\Class\{F01A9D53-…}\0000` 的 `InfPath/DriverVersion` + 设备实例 `ConfigFlags |= 0x20`（`rebind-live.ps1` 已封装）；
   (b) **直接替换** `System32\drivers\*.sys` 与 DriverStore 里的同名文件（`offline-install-stack.ps1`，最省事、不碰 PnP）。
3. **PnP 热装/重启设备会卡死整机**（历史上多次）：只做 `pnputil /add-driver` 暂存，或离线换文件。
4. **Offline hive 需要管理员**：`reg load` 前要 `takeown`/`icacls`；备份文件写到 `F:\Windows\System32\config\SYSTEM.bak-*`。
5. **本机网络**：Clash 的 TUN 会造成"假连通"；GitHub API 时通时断，raw.githubusercontent 也不稳定。
6. **UEFI 屏幕最后一定被 wifiprep 覆盖**（`\SwitchWOA\Wifi\WifiPrepare.efi`，hold 6 s），别指望 UEFI 打印。
7. **中文编码**：很多脚本/文档混用 GBK/UTF-8，`Get-Content` 输出乱码属正常，用 `read` 工具看文件。
8. **平台 I/O 失败会被锁存**：`platform_io.c` 的 `failure()` 把第一个失败写进 `Platform.IoStatus`，
   而 `platform_adapter.c` 在硬件成功后仍会用它改判整个启动。**任何诊断读取都必须使用桥支持的地址**，
   否则会出现"硬件明明成功却报 INVALID_PARAMETER"（`BcDetail=0` 是识别它的关键）。
9. **先验 `BcDetail`/`Bc` 再怀疑硬件**：`TgGpuBc(6,status)` 记录的是 `TgGpuStartHardware` 的真实返回；
   `HardwareStatus` 可能是被后续步骤覆盖后的值。
10. **重启后先看 `HardwareStage` 与 `Step` 的组合**：`HardwareStage` 停在 1xx/2xx/3xx 表示停在上电/时钟/执行阶段；
    `gpu-run.txt` 的最后一个 `Step` 定位当前子步。

---

## 9. 关键文件索引

- 驱动上电逻辑：`erista-gpu\src\mesa-switch-woa\tegra-x1-wddm\platform\t210\t210_power.c`（本文件就是这两天的实验主战场；
  `write_dump_file()` 的记录上限已从 64 提到 192，`gpu-run.txt` 现在能完整落到最后一个 step）
- 时钟逻辑：同目录 `t210_clock.c`（GPCPLL 在孔径内：`GPU+0x137000`）
- 资源契约：`platform\t210\t210_resources.h` + `windows\resources.c`（**窗口数/尺寸必须与 DSDT 完全一致**；
  当前 `0x7000E400/0x2E0` 一个窗口覆盖 PG_MASK/PWR_DET/GPU_RG_CNTRL）
- ACPI：`WoA\build\NintendoSwitchPkg\AcpiTables\Dsdt\Dsdt.asl`（GPU0 / FAN0 / PWRB）
- UEFI 上电尝试：`WoA\build\NintendoSwitchPkg\Drivers\PmicDxe\Pmic.c`（含 ReadyToBoot 回调与 GPIO6 早期拉高、
  TD toggle、`0x34` 清钳位、`[gpupower]` 屏显）
- switchfan：`WoA\erista-gpu\switchfan-t210\switchfan.c`（`TG_GPU_IOCTL_PMIC 0x00222410`，0x1C 电压 + 0x3C 原始寄存器）
- 历史与结论：`WoA\erista-gpu\STATUS-GPU0-BOOT.md`（**10.8/10.9 是前两天的核心结论，第 11 章是本轮的更正**）

---

## 10. 下一步优先级（按重要度排序，交接给下一个会话）

> 前提：**内核侧 bring-up 已完成**（Code 0）。UMD 仍是 2 KB 占位，所以凡是要"真正渲染"的验证都得等朋友那边的 UMD/ICD。
> 下面 P0/P1 全部**不需要 UMD**，可以独立推进并真机验证。

### P0 —— 摸清算力天花板（第 1 项已完成，剩下第 2 项）

1. ✅ **已完成（2026-11-21，见 STATUS 第 16 章）：GPU 逐档提频上机。**
   档位改由注册表 `Parameters\BootGpuKhz` 选择（不重编驱动）：460800 → 691200 → 768000 → 844800
   逐档重启实测，**四档全部通过**，实测频率 = 目标、电压 = CVB 预测值（868,750 / 1,000,000 / 1,050,000 / 1,106,250 µV）。
   - 结论：**这台 Erista 的零售天花板就是 844.8 MHz**；921.6 MHz 需要 >1,113,000 µV，属 DTS/电压域改造，不要再试。
   - 0.1.0.111 已收口 `required_uv == 0` 的回退漏洞（有 speedo 时直接 `RAIL_ERROR`），
     由 `tests\test_clock_t210_host.c` + `tools\run-host-tests.ps1` 在 PC 上验收，不必占用开机轮次。
   - 仍待做：`windows/render_miniport.c` 的 `BootGpuTargetKhz` 还是编译期常量（实际档位看 `ClkNote3`），
     建议与 P3-8 一起改成 `Status.target_khz`。
2. **上屏链路微基准**（不需要 UMD，也不需要游戏）——**下一轮从这里开始**：
   用 KMD 自带的 GPU 诊断入口跑 720×1280 全屏操作各 N 次并计时：纯 blit、半转（`present_halfturn`）、缩放。
   产出：每次耗时 → 换算"上屏链路理论最大 fps"。
   - 入口：`core/present_blit.c`、`core/present_halfturn.c`、`tg_gpu_execute_clear()`/`tg_gpu_execute_3d()`（`core/execute.c`）。
   - 目的：直接回答"20 fps 是算力不足还是上屏链路代价"。
   - **已勘查到的前提**（省下一轮）：当前构建**没有**定义 `TG_GPU_PANEL_WDDM`，所以 `TgGpuDdiPresent`
     （`windows/present.c`，CE 2D 条纹 blit/半转）根本没注册；`tg_gpu_execute_*` 全是"一次性引导"，
     每次调用都会重做通道/MMU 引导，**不能直接循环计时**。要拿到"每帧代价"需要一个**新的可重复提交循环**：
     复用 `tg_gpu_channel_publish()` 的 GP_PUT + 内存 fence（`BAR1 + FENCE_VA`）语义，在已绑定的通道上
     连续提交 N 个 `tg_gpu_build_clear_packet()`/`tg_gpu_encode_present_copy()` 包并用
     `KeQueryPerformanceCounter` 计时，结果照 `record32` 模式写注册表 + `gpu-run.txt`。
     另外 720×1280×4 需要 ~3.6 MB 目标面，现有 ARENA 只有 512 KB（`TG_GPU_ARENA_BYTES`），
     要么放大 arena，要么单独分配一块 + 建 VA 映射。**这是本轮未做的原因**：它是一次真实的
     "新增提交路径"改动，必须能连续开机调试，不能在收尾时赶工。
   - ✅ **不需要驱动的那一半已经量完（STATUS 第 17–18 章）**：
     - 显示栈是 `Microsoft Basic Display Driver` @ **1024×768**，我们的 GPU 只是 render-only、**不参与上屏**；
     - **CPU 实测跑在 1750 MHz**（`QueryThreadCycleTime` 测的，并做过 `Sleep()` 反证；不是 1024 MHz 档），
       所以下面的数字已经是 boost 档下的结果，没有"提 CPU 档"这种便宜可捡；
     - 单帧 1280×720（3.6 MB）单线程代价：帧拷贝 2.3–2.5 ms（DRAM 上限 ~1.55 GB/s，**4 线程无收益**）、
       SrcOver 混合 6.6–7.3 ms、**90° 旋转 朴素 20–24 ms / 分块 11–14 ms / 指针递推 5.15 ms（4 线程 2.7 ms）**；
     - **"缩放+旋转一趟做完"是死路**：两种写法都 18–23 ms（读放大 ~16×）；分开做 5.15+2.36≈7.5 ms。
   - **结论**：朴素实现"旋转+融合缩放+拷贝"≈37–41 ms（24–27 fps），**指针递推实现只要 ≈9.9 ms**，
     而 **DWM 合成是关闭的、帧缓冲 1024×768 32bpp、模式 60 Hz → 每帧预算 16.7 ms**
     （STATUS 第 19 章）→ 优化后的 CPU 路径**装得进 60 Hz 预算**，20 fps 里至少 25 ms/帧是纯实现浪费，
     既不是 GPU 算力，也不是"到屏"慢。
   - 另外两条别踩的坑（都在 STATUS 第 19 章有数据）：① "写屏幕 DC 很快"是假象——帧缓冲写入只落在缓存/写合并区，
     DC 扫描输出是异步的，用户态**测不到真实 present 代价**；② GDI+ `DrawImageUnscaled` 全屏 26.6 ms/帧
     是它自己做格式转换，**不要拿它做全屏 present**。
   - 机器上**没有任何 Tegra 显示/面板驱动**（`drivers` 里只有 `tegra210gpio.sys`/`tegra210i2c.sys`/
     `tegragpurender.sys`；`oem*.inf` 无 Display 类）→ "打开现成的硬件旋转通路"不存在，硬件旋转必须新写显示小端口。
   - 下一步（给上屏链路/UMD 的改法，按收益排序）：① 旋转换成 `op_rot90_fast()` 的写法（目的连续写、源按 stride
     递推，别在内层做 `x*dw+(h-1-y)` 的 64 位乘加）；② 旋转与缩放**分开**，别融合；③ 旋转按目的行切 4 段多线程；
     ④ 减少每帧整帧拷贝次数；⑤ 最后才是下沉到 DC 硬件（P2-6，需要新驱动）。
   - 参考实现已经写好、可移植（C99、无平台依赖）：`tests\screenbench.c` 的 `op_rot90_fast()`、`op_scale_rot_fast()`。
   - 仍然缺、必须改驱动才能拿到的：GPU 提交/完成延迟、真实 present/fence 时间（用户态测不到，见上）。
   - 工具：`WoA\erista-gpu\tests\screenbench.c` + `tests\cpuprobe.c` + `tools\build-screenbench.ps1`
     （ARM64 原生、无 CRT、免签名），卡上 `C:\SwitchWOA\screenbench.exe`、`cpuprobe.exe` 可随时复跑。

### P1 —— 把"失败"变成"可取证"（等朋友那边 WC3 复现时一次抓到现场）

3. **T210 first-fault 取证**：照 `t210_power.c` 的 `record32/record_step` 写穿模式，
   在提交路径记录：首个 fault 的通道 ID、fence 期望/观测、PBDMA 状态、USERD GP_PUT/GET、以及触发 DPC/ISR 的寄存器。
   - 参考：`docs/RECOVERY_TRACE.md`（T214 已有）、`core/fault.c`、`windows/recovery.c`。
   - 验收：能人为注入一个非法命令流，挂死后 `gpu-run.txt` 能读出"哪条命令、哪个 fence"。
4. **区分 WHEA 与 DPC_WATCHDOG**：WC3 读条死机属哪类？前者偏总线/电源，后者偏提交卡住。这条决定后面查哪边。

### P2 —— C 阶段（上屏链路）代码先行

5. `PresentRotate=0/1` A/B 开关 + 每帧计时（合成/blit/fence 等待）；
6. 把旋转、缩放从软件半转下沉到 DC（`t210_dc.h`、`windows/panel_scanout.c`、`windows/panel_vidpn.c`）；
7. 直翻/MPO 契约按 `docs/MPO_CONTRACT_AUDIT.md` 的清单逐条对；避免每帧回读。

### P3 —— 固化今晚的成果（低风险，随时可做）

8. 把工作档开关写成驱动默认值：`HardwareStartLimit` 默认 5、`RgWrite` 默认 1、`ClampNeedsTd` 默认 0、
   `PriRing` 默认 1、`RailSettleMs` 默认 25 —— 目前 `HardwareStartLimit` 缺省仍是 0（=不启动硬件）；
9. 删掉诊断期加的探针（`GrFailMask`/`GrCmdHandle`/`McEnableAfter`/`RwFirst` 等）或统一放到开关后；
10. 启动自检断言：每次开机自动判 `HardwareStatus/ClockResult/ExecutionResult/GrStage/StatusWorker` 是否全绿。

### 本轮新增的三条硬规则（务必遵守）

- **诊断读取只能用 PMIC 桥支持的寄存器**：MAX77621 `0x00/0x01/0x06`、MAX77620 `0x3C/0x40`。
  读其它地址会返回 `INVALID_PARAMETER`，而 `platform_io.c` 的 `failure()` 会**锁存第一个失败**，
  把已经成功的启动整体判失败（今晚就踩过这个坑）。
- **分区 13（3D1）禁止 toggle**：实测会直接把总线挂死（`Step 0x19` 之后 WHEA）。
- **PLL 页读写必须在 MC 排空之后（+20 ms）**，更早会挂总线。

---

## 11. 本轮（2026-11-21 下午）改动清单与审核状态

### 11.1 改了什么（两处驱动功能改动 + 两个免驱动工具链 + 两个测试/基准）

| 文件 | 冻结 SHA256（前 32 位） | 内容 |
|---|---|---|
| `platform/t210/t210_clock.c` | `1457A0E0B3FCEBE72376BD0D06DC0CCC` | ① `tg_gpu_clock_start()` 的目标频率白名单换成"OPP 表内任意一档"；② 新增 `ClkNote3/4/6`（目标 / CVB 电压 / speedo）、`ClkNote5`（external_calibration）、`ClkNote7`（实测）与 `mark 84`；③ **CVB 上限守卫**：有 speedo 且 `required_uv==0`（超出 `max_mv`）直接 `RAIL_ERROR`，不再回退到在飞电压 |
| `windows/hardware.c` | `D9CA955065F46946807E72AA81C09EB9` | 新增 `hw_boot_khz()`（读 `Parameters\BootGpuKhz`，76800..921600 且为 76800 整数倍才接受），`Status.target_khz` 与两处 `tg_gpu_clock_start()` 用它；新增记录 `BootGpuKhzReq` |
| `WoA\erista-gpu\tests\test_clock_t210_host.c` | `1AC097D837253C419FDA80627B064220` | **新增** PC 侧主机测试：include 真实 `t210_clock.c`+`t210_opp.c`，四档电压/频率/`ext_cal`、921600 拒绝、表外频率 `BAD_STATE`、低档 `VMIN` |
| `WoA\erista-gpu\tools\package-kmd.ps1` | `7C8F2DEF3BD11391E05DCCE5C3BF46A1` | **新增** 打包+签名一步到位（先签 sys → Inf2Cat → 再签 cat） |
| `WoA\erista-gpu\tools\run-host-tests.ps1` | `4049950DEBDEB0167AB18F7064B38A6A` | **新增** 编/跑主机测试（clang-cl + `/nodefaultlib`，只链 Kernel32） |
| `WoA\erista-gpu\tests\screenbench.c` | `DF491710C4AEFDCC6F4551A9CCBEE6BF` | **新增（19:1x，免驱动那一半）** 上屏链路 CPU 基准：1/4 线程的帧拷贝、90° 旋转（朴素/分块）、缩放、SrcOver 混合、"旋转+缩放+拷贝"每帧总代价；ARM64 原生、无 CRT、免签名 |
| `WoA\erista-gpu\tools\build-screenbench.ps1` | `B5969377B545DB088338EF0D18D0AED3` | **新增** 编这个基准（`--target=aarch64-pc-windows-msvc` + `lld-link /nodefaultlib`，只链 `um\arm64\kernel32.lib`） |
| `tests\arm64-bin\screenbench.exe` | `0B86084B1FA9D8EFFC230827CCC5FDE0` | 产物（21,504 B；含指针递推旋转/融合缩放旋转/4 线程对照；20:2x 修正了块内填充方向，耗时不变 5.12 ms），卡上副本 `C:\SwitchWOA\screenbench.exe` 同哈希 |
| `WoA\erista-gpu\tests\present_ref.c` | `72BEE301A913EF8E10F5C1324C87AF1C` | **新增（A2 交付物）** 可移植 C99 面板路径旋转/缩放：`tg_present_rot90()` / `tg_present_rot90_range()`（多线程切片）/ `tg_present_scale_nearest()`，含像素级自测 |
| `WoA\erista-gpu\tests\present_ref.h` | `2697D7439CFC620C4082185321888315` | 上述接口 + 四条提速规则的注释（内层无乘法、按目的行切分、目的端按 128 B 连续写、旋转与缩放分开） |
| `windows\render_miniport.c` | `0D50E9DB6623A2AB8D90F7259B99A4C4` | **B2**：启动末尾写 `SelfCheck`（1=全绿 / 2=故意闸住 / 0=失败） |
| `windows\platform_adapter.c` | `195EBE1365372964BD3D1A231E213282` | **B1**：`hw_start_limit()` 缺省 0 → 5（完整 bring-up） |
| `windows\hardware.c` | `6A62D85019389837BB2785B3C19DF573` | 仅注释（指向新缺省值）；另有 P0 的 `hw_boot_khz()` / `BootGpuKhzReq` |
| `kmd-package\tegragpurender.sys` | `0C78DA6AD7A575B40D60209FFA4B2FBA` | 0.1.0.112 签名包（卡上同哈希） |
| `WoA\erista-gpu\tests\cpuprobe.c` | `8CBF326757788F627CFB046AF845D5E0` | **新增** CPU 主频探针（`QueryThreadCycleTime` + `Sleep()` 反证），实测 ≈1750 MHz |
| `tests\arm64-bin\cpuprobe.exe` | `87A16163B6E82D4C4749CCEAF56D29C9` | 产物（5,632 B），卡上 `C:\SwitchWOA\cpuprobe.exe` 同哈希 |

`D:\paopao\.tmp` 下另有本轮临时工具（不入库）：`set-boot-khz.ps1`、`pull-run.ps1`、`rsh-retry.ps1`、`rsh.ps1`。

> 注意：`platform/t210/` 在 `mesa-switch-woa` 仓库里整体是**未跟踪**目录（`??`），`windows/hardware.c` 相对
> HEAD 的 diff 里也混着前面几轮的未提交改动。所以本轮只记录上面这些文件的哈希，没有做组合提交。

### 11.2 Gemini 独立审核：**不适用（用户 2026-11-21 明确取消）**

`erista-gpu\src\mesa-switch-woa\AGENTS.md` 里那条"每轮新增代码交 Antigravity CLI 的 Gemini 4 Carbon High 审一次"
是**朋友那个项目**的规矩；用户已明确**本仓库/本轮不需要**。本机也确实没有 `antigravity`/`gemini` 可执行文件。
→ 不要再为这条安排轮次；11.1 的哈希留作改动记录即可。

---

## 12. 本轮之后还能做什么（按"要不要动驱动"分类，2026-11-21 19:5x 整理）

### A. 不需要动驱动、现在就能做

| # | 事情 | 价值 / 现状 |
|---|---|---|
| A1 | ✅ **已完成（首轮）**：soak 任务运行中，20:19 起样本全绿 ｜ **稳定性 soak**：`C:\SwitchWOA\soak.ps1` 每 30 s 记一行（设备状态/ProblemCode/`HardwareStatus`/`ClockResult`/`BootGpuMeasuredKhz`/`Step`/`GrStage`/`StatusWorker`/可用内存/WHEA·bugcheck·Kernel-Power 计数）到 `C:\SwitchWOA\soak.log`，默认 40 分钟 | **本轮已在跑**（19:50 启动）。用来回答"当前档位长时间运行会不会掉驱动/出 WHEA"，是 P1-4 的前半 |
| A2 | ✅ **已完成**：`tests\present_ref.c/.h`（可移植 C99，`tg_present_rot90`/`_range`/`tg_present_scale_nearest` + 像素级自测，已接入 `run-host-tests.ps1`） | 上屏链路/UMD 可直接拿走；见 STATUS 第 20 章 |
| A3 | **高温/风扇联动复核**：GPU 460.8 与 844.8 档位下连续跑 benchmark，读温度（ACPI thermal zone 或 switchfan IOCTL）看是否掉频 | 只读，能补上"高档位长时间是否安全"这条 |
| A4 | **844.8 MHz 档 + CPU 满载下的 soak** | 上一条的加强版；若掉频就把 `BootGpuKhz` 定在 460800（当前值） |
| A5 | UMS 下核对启动项与回滚镜像（`bl33-v5x/v6x/v67/v68-held.bin`、`install-safe.cmd`、hekate 自动启动项） | 纯读卡，确认随时可回退 |

### B. 需要改驱动，但改动小、风险低（各几十行）

| # | 事情 |
|---|---|
| B1 | ✅ **已完成（0.1.0.112，真机验收）**：`HardwareStartLimit` 缺省 0 → 5，其余默认本已是工作档；删光注册表开关后启动仍 `SelfCheck=1`/Code 0（STATUS 21.2） |
| B2 | ✅ **已完成（0.1.0.112）**：`SelfCheck` = 1/2/0（全绿/故意闸住/失败），实测读出 `SelfCheck=1` |
| B3 | P3-9：清理诊断期探针（`GrFailMask`/`GrCmdHandle`/`McEnableAfter`/`RwFirst`…）或统一收进开关 |
| B4 | `windows/render_miniport.c` 的 `BootGpuTargetKhz` 改成记 `Status.target_khz`（现在写的是编译期常量 460800，实际档位要看 `ClkNote3`） |

### C. 需要改驱动，工作量中/大

| # | 事情 |
|---|---|
| C1 | **P0-2 剩余**：可重复提交循环（`tg_gpu_channel_publish` + 内存 fence + `KeQueryPerformanceCounter`），量 GPU 每帧代价；需要放大 arena 或单独分配 3.6 MB 目标面 |
| C2 | P1-3：提交路径 first-fault 取证（通道 ID / fence 期望观测 / PBDMA / USERD GP_PUT-GET） |

### D. 需要新驱动或固件（收益最大，也是唯一的"真正提速"路线）

| # | 事情 |
|---|---|
| D1 | **显示小端口 + DC 硬件旋转/overlay**：机器上现在只有 `Microsoft Basic Display Driver`，`drivers` 里没有任何 Tegra 显示驱动，`oem*.inf` 也没有 Display 类 → 硬件旋转不是"打开开关"，是**新写显示驱动**。**关键细节（STATUS 22 章）**：T210 上横屏是靠 Window A 的 **column scan**（L4T 表声明 column scan + packed-format rotation，只对 Display A Window A），且**必须 block-linear 扫描面**，不能沿用现在的 pitch-linear/2880 布局；`docs\PORTING.md` 293-299 记录 **2026-11-14 已把 DC column scan 与 decrement transform 移除**、改用 GPU prerotation，物理输出至今未验证。KMD 侧 `panel_scanout.c::storage_matches()` 目前只接受竖屏 identity，`t210_dc.h` 的 transform 位只用于清除继承值 |
| D2 | 让 GPU 参与 present（B/C 阶段的合成路径，配合朋友的 UMD/ICD） |

> 推荐顺序：**A1（已在跑）→ A2 → B2 → B1 → A3/A4 → C1 → D1**。
> 前四项都不需要重新思考架构，且能把"20 fps 是上屏链路 CPU 代价"这条结论变成实际收益。
