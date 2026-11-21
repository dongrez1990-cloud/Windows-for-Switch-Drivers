# SWOA GPU0 (T210/Erista) 进度与交接

> **先读交接文档：`WoA\erista-gpu\HANDOFF-20261120-GPU0.md`**（现状/已证伪结论/三条路线/环境坑）
>
> **下次从这里继续（2026-11-20 停点）**
> - 根因已确定：**VDD_GPU 使能 = MAX77620 GPIO6**，Hekate 交接时设成输入（轨关）；KMD 只写了 MAX77621 电压寄存器，EN 脚从未拉起 → TD 分区永不上电 → 孔径一读即挂（WHEA / DPC_WATCHDOG）。
> - 下一步三处改动（详见 10.8 节）：FAN0 `_CRS` 加 `I2cSerialBusV2(0x003C, ..)` → switchfan 第三 target + 按地址路由 → KMD 先 `GPIO6=0x09` 再写电压。
> - 卡上现状：固件 **v71**（`A5D62E6C…`）、KMD 文件 **0.1.0.91**、参数 `Limit=1 / Mask=0x80 / RgWrite=0 / ClampNeedsTd=1 / ApertureProbe=0`，设备 Code 0、系统稳定。
> - 回滚：`E:\switchroot\winx-gpu0\bl33-v7x-held.bin` 系列 + `00-swoa-v46.ini` 未动。
> - 取证：关机 + UMS 后直接读 `F:\SwitchWOA\gpu-run.txt`（驱动每步写穿，无需 SSH/管理员）。
# v49 = 可用启动链（v47 配方）+ GPU0 ACPI —— 构建与部署记录

> 目的：让**带 GPU0（`ACPI\SWOA0F01`）的固件也能正常进 Windows**。
> 上一份"裸树构建"的 bl33（sha `43C913E0…`）在 `UsbPowerAuditDxe` 走到终态
> （`R309 FINAL SUMMARY / STOP_POINT=BM92T_HOST_GATE`）不再启动 Windows，
> 因为它缺 v46/v47 的启动链改动。

## 1. 关键认知（本轮确认）

| 事实 | 依据 |
|---|---|
| 能进 Windows 的是 **v47 二进制**（`1CCEC8E9…`），v46 目录里放的其实是 v47 | `switchjoycon\HANDOFF.md:126-127`、`WoA\rebuild\out\bl33-swoa-v47.bin` |
| v46/v47/v48 的**确切构建配方**是 `WoA\build\build-variants.sh` | 该脚本 v46/v47/v48 三行 flag 完全一致；v48 仅多加 `SWITCH_BDS_BLIND_WAIT=1` |
| 构建树是 `WoA\build\`（`edk2` + `NintendoSwitchPkg`），**不是** `WoA\winx` 原始树 | v47/v48 的 FD 与 `WoA\build\edk2\Build\...\TEGRA210_EFI.fd`（`800148E3…`）一致 |
| Windows 由 ESP 上的 `\EFI\Microsoft\Boot\bootmgfw.efi` 启动，WiFi 预载件在 `\SwitchWOA\Wifi\`（`WifiPrepare.efi` + brcmfmac4356A3-pcie.*） | `V:\EFI\Microsoft\Boot\bootmgfw.efi` 1,799,024 B |
| `补丁-Erista-WiFi\PATCH.md`（11/7 写的建议）**已被上游实现**，但旗标名不同：现状用 `SWITCH_WIFI_ERISTA`（PlatformBm.c:588），不是 `SWITCH_ERISTA_HOME` | `WoA\winx\NintendoSwitch.dsc:58-81`、`Library\PlatformBootManagerLib\PlatformBm.c:588` |

## 2. 本轮改动（只加 GPU0，不动其它）

改的是构建树 `D:\paopao\WoA\build\NintendoSwitchPkg\`：

1. `NintendoSwitch.dsc`
   - 新增 `DEFINE ASL_GPU0_FLAGS`：`!ifdef SWITCH_GPU0` → `-DSWITCH_GPU0`
   - `GCC:*_*_AARCH64_ASLPP_FLAGS` 列表首位加入 `$(ASL_GPU0_FLAGS)`
2. `AcpiTables\Dsdt\Dsdt.asl`
   - `Scope(_SB)` 末尾新增 `#ifdef SWITCH_GPU0` 的 `Device (GPU0)`：
     `_HID="SWOA0F01"`、`_UID=0`、`_STA=0x0F`；
     `_CRS` = 0x57000000/16M + 0x58000000/16M + 0x538F0000/4K + CAR 0x60006000/4K
     + GSI 0xBD/0xBE（SPI157/158）+ `I2cSerialBusV2(0x001C → \\_SB.I2C5)`（MAX77621 VDD_GPU）

构建脚本：`WoA\build\build-gpu0.sh`（复制 v47 的 24 个旗标 + `-D SWITCH_GPU0=1`）

## 3. 产物

| 文件 | 大小 | sha256 |
|---|---|---|
| `WoA\rebuild\out\bl33-swoa-v49.bin` | 1,048,576 B | `014C7B28C29B9A70B1B2972DE31B2CBB669C64A83A9C1F8FC075B52A5609165B` |
| `WoA\rebuild\out\fv-v49\Dsdt.aml` | 3,097 B | 含 `SWOA0F01`×1、`GPU0`×1，FAN0/PWRB/RTC0/BAT0/AUD0/BTH0 均在 |
| 对照 v47 | — | 二进制差异只有 2 段（0x8d0c 起 15 B、0x9a10 起 816 KB＝FV 压缩流因 ACPI 变化整体重排），无其它改动 |

`make-bl33.py --verify`：静态检查通过；FV 占用 81%（0xd10d8 / 0xff000）。

## 4. 部署状态（卡上，2026-11-18 夜）

- `V:\switchroot\winx-gpu0\bl33.bin` = **v49**（hash 已回读一致 `014C7B28…`）
- 旧版（裸树 + GPU0，不进 Windows）备份为 `V:\switchroot\winx-gpu0\bl33-prev-gpu0-acpi.bin`
- Hekate 项 `V:\bootloader\ini\06-swoa-gpu0.ini`：
  ```
  [SWOA GPU0 ACPI SWOA0F01 test]
  l4t=1
  boot_prefixes=switchroot/winx-gpu0/
  id=WINXGPU0
  cpu_oc=1785600
  ```
- 回滚：菜单选 `[SWOA v46 JoyCon rail (IRQ 69/78) <= 1st]`（`winx-v46`，未动）

## 5. 下一步（步骤 2 验收）

1. 重启 → Hekate → 选 `SWOA GPU0 ACPI SWOA0F01 test`；
2. 进 Windows 桌面后经 SSH 查：
   ```
   reg query HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SWOA0F01
   ```
   **预期**：出现 `ACPI\SWOA0F01\0`，未知设备（Code 28）——这就是 ACPI 枚举通过；
3. 之后才做 KMD（`KMD-BRINGUP-SPEC.md`）与执行/回读（`STEP1-CHECKLIST.md`）。

---

## 6. 步骤 2 验收结果（2026-11-18 22:01，实机）

`SWOA GPU0 ACPI SWOA0F01 test` 项成功进入 Windows 桌面（本次 Boot 21:57:49，
卡上 bl33 = v49 `014C7B28…`）。设备枚举证据（SSH 注册表直读）：

```
HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SWOA0F01\0
  HardwareID   ACPI\VEN_SWOA&DEV_0F01, ACPI\SWOA0F01, *SWOA0F01
  ConfigFlags  0x40
Get-PnpDevice -InstanceId 'ACPI\SWOA0F01\0'  -> Status=Error
DEVPKEY_Device_ProblemCode = 28        (CM_PROB_FAILED_INSTALL：无匹配驱动)
```

同机其余 SWOA 设备（0101/0201/0301/0401/0501/0601/0801/0901/0B01/0C01/0D01/0E01/0E02/0F02）不变。
→ **步骤 2「ACPI GPU0 + 枚举」通过**；下一步是 KMD（把 Problem 28 变 Code 0）。

## 7. KMD 侧关键发现（2026-11-18 夜，为步骤 3 铺路）

1. **不需要 dxgkrnl.lib**。上游 `tegra-x1-wddm/tools/build.py:425-431` 的 miniport 链接行是：
   ```
   lld-link /driver /subsystem:native,10.0 /machine:arm64 /entry:GsDriverEntry
            /include:_load_config_used /nodefaultlib /release /opt:ref
            /section:.text,ERP /section:.rdata,RP /section:.data,RWP /section:.pdata,RP /section:INIT,ERD
            TegraGpuCore.lib render_miniport.obj
            displib.lib ntoskrnl.lib hal.lib bufferoverflowfastfailk.lib libcntpr.lib
   ```
   库里用的是 **`displib.lib`**（KMD 侧的 DDI 胶水，提供 `DxgkInitialize`/`DxgkInitializeDisplayOnlyDriver`/
   `DxgkUnInitialize`/`DxgkIsFeatureEnabled2`），本机 WDK 26100 `Lib\10.0.26100.0\km\arm64\displib.lib`
   （715,070 B）齐全；`ntoskrnl/hal/bufferoverflowfastfailk/libcntpr` 也都在。上游/本机都没有
   单独 `dxgkrnl.lib`，之前"缺 dxgkrnl.lib 所以不能本地链接"的结论作废。
2. **上游工作树里已有 T210 端口**（上一轮写的，未提交）：`src\mesa-switch-woa\tegra-x1-wddm\platform\t210\`
   有 `t210_power.c / t210_clock.c / t210_gr.c / t210_frontend.c / t210_acr.c / t210_opp.c / t210_pmu.c /
   pmic_max77621.h / t210_resources.h / t210_dc.h`，并有 `t214_*.h` 转发垫片；`windows/` 侧 78 个文件
   已改为 `../platform/t210/...` 引用（`git status` 83 项未提交）。
3. `tools/build.py --render` 还要求外部固件目录：`--gr-firmware`（NETB_img.bin/fecs.bin/gpccs.bin）与
   `--acr-firmware`（nv_acr_ucode_prod.bin/pmu_bl.bin/gpmu_ucode_image.bin/gpmu_ucode_desc.bin/
   pmu_sig.bin/fecs_sig.bin）。本机暂未找到这些文件（下一步从实机/switchroot 侧取）。
4. 本机可行性：`clang-cl --target=aarch64-pc-windows-msvc /kernel` + WDK 26100 头已能编译
   `windows/render_miniport.c` 之前的对象（当前因工作树里 panel.h 已切到 t210 头、需按 t210 平台一起编）。
5. 已生成但**不采用**的试验品：`WoA\erista-gpu\wdk\dxgkrnl.lib`（用设备 `dxgkrnl.sys` 的 319 个命名导出
   生成，该 .sys 并不导出 DxgkInitialize，故无用）；保留 `dxgkrnl.sys`、`.def` 供查证。

### 下一步（步骤 3 入口）

1. 取 GR/ACR 固件文件（实机 `C:\Windows\System32\drivers\SwitchWOA\` 或 switchroot 卡）；
2. `python tools/build.py --render --wdk-include <kits>\Include\10.0.26100.0 --sdk-include <kits>\Include\10.0.26100.0 --gr-firmware … --acr-firmware … --out <out>`；
3. 修 T210 端口的编译/链接错误，直到 `tegragpurender.sys` 出来；
4. 签名 + INF/CAT 打包 → 装到 `ACPI\SWOA0F01`（期望 Code 0）→ 再做执行与回读验收（STEP1-CHECKLIST.md）。

---

## 8. KMD（tegragpurender）首装上机记录（2026-11-18 22:0x–22:2x）

### 构建链（本机全通）

- `WoA\erista-gpu\tools\build-kmd-t210.ps1`：clang-cl 23.1.5（`--target=aarch64-pc-windows-msvc /kernel`）+ WDK 26100 头 + lld-link。
  - 源：core/*（18 个）+ platform/t210（t210_frontend/gr/acr/pmu/power/clock/opp）+ platform/t214（mmu/gr_context/interrupt/dc/nvdec 仍用 T214 实现）
    + windows/*（39 个，含 nvdec_adapter/backend/memory）+ mesa `nouveau/horizon/nouveau_horizon_gm20b.c` + mesa `nouveau/nvdec/*`（4 个）
  - 链接用 **`displib.lib`**（不是 dxgkrnl.lib）：`displib.lib ntoskrnl.lib hal.lib bufferoverflowfastfailk.lib libcntpr.lib`
  - 产物：`kmd-build\tegragpurender.sys`（219,136 B，ARM64，仅 import ntoskrnl/HAL——Dxgk* 由 displib 静态胶水提供）
- T210 端口需要的两处代码补齐（本轮改）：
  1. `platform/t210/t214_constants.h`：删除 50 条 `T214_DC_*` 别名（DC 头已回到 `platform/t214_dc.h`，重复定义会 `-Werror`）；
  2. `platform/t210/t210_clock.[ch]`：新增 **T214 兼容的 hiopt 面**（`TG_GPU_HIOPT_OPP_COUNT=12`、`hiopt_freq_khz/index/uv`、`thermal_range`、`emc_floor_uv`、`required_uv`），
     用 T210 OPP 梯形（76800..921600）+ CVB + EMC floor + Vmin 590 mV/OC 上限 900 mV，`windows/dvfs.c` 不改一行。
  3. `windows/{panel.h,allocation.c,present.c,resources.c}`：DC 头回到 `../platform/t214_dc.h`（T210/T214 DC 寄存器同构）。

### 签名 / 安装

- 自签证书 `CN=SWOA GPU Test Signing`（thp `08C561EA…86EC`，10 年有效；设备 testsigning=Yes）
- `Set-AuthenticodeSignature` 签 SYS（嵌入签名 1432 B）与 CAT；`Inf2Cat.exe`（WDK nuget 的 x86 版）生成 `tegragpurender.cat`
- 设备端：证书装入 Root+TrustedPublisher → `pnputil /add-driver … /install`（独立进程）
  → **`oem13.inf`，设备 `ACPI\SWOA0F01\0` Status=OK / Class=ComputeAccelerator / Problem 0**，服务 `tegragpurender` RUNNING

### 驱动自报状态（`HKLM\SYSTEM\CurrentControlSet\Services\tegragpurender\Parameters`）

| 值 | 含义 |
|---|---|
| `InitializeStatus = 0` | DxgkInitialize 成功 |
| `ResourceStatus = 0xC0000102` | `TgGpuDdiStartDevice` 失败（资源契约不满足） |
| `HardwareStatus = 0xFFFFFFFF` | 尚未进入硬件 bring-up（早退） |
| `BootGpuTargetKhz = 729600` | 构建时 TG_GPU_RENDER_KHZ |

失败根因（代码级）：`windows/resources.c:184-265` 要求 **_CRS 精确提供 11 个内存窗口**（= TgGpuApertureCount）：
`0x57000000/16M`、`0x58000000/16M`、`0x538F0000/4K`、`0x60006280/0x18`、`0x6000667C/4`、`0x7000E6D4/4`、
`0x70019970/8`、`0x70019AA8/8`、`0x70000804/4`、`0x7000F930/4`、`0x7000FB04/4`，外加 ≥2 条 level 中断（stall/nonstall），
多一个或少一个都返回 `STATUS_DEVICE_CONFIGURATION_ERROR`。v49 的 `_CRS` 只有 4 个窗口（把 CAR 整块 0x60006000/4K 当作窗口），必然失败。

### 已部署 v50（资源已按契约修正）

- `Dsdt.asl` GPU0 `_CRS`：三窗口 + 8 个窄 power/ID 窗口 + GSI 0xBD/0xBE + I2C5@0x1C；**注意 CPUC 的 `0x60006000/4K` 已恢复**（本轮误改过一次，已修）
- `bl33-swoa-v50.bin` sha256 `C281CC7000BE45D5465159D9DF40E7070321654114943640CF8F1A79B23A7433`，已写入 `V:\switchroot\winx-gpu0\bl33.bin`（v49 备份为 `bl33-v49.bin`）

### 后续两个已知硬依赖

1. **轨主（rail owner）**：KMD 通过 `\Device\SwitchBacklight` + IOCTL `0x00222410`（`TG_GPU_IOCTL_PMIC`）向 SwitchFan 请求 GPU 轨。
   本机 Erista 的 `switchfan.sys`（1.0.0.22，26536 B）**没有**该 IOCTL；`WoA\wd057\drivers\SwitchFan\src\switchfan.c:315` 的版本才有（但它按 T214/MAX77812 M1 写）。
   → 需要为 Erista 做 MAX77621（I2C5 0x1C，0x06 EN / 0x23 VOUT，6.25 mV）的 PMIC 桥，或用 `platform/t210/pmic.c` 那套 KMDF SPB 封装。
2. **GR/ACR 固件**：`NETB_img.bin/fecs.bin/gpccs.bin` + ACR 六件套未取得（本机/实机都没有；当前包内是 4 KiB 占位文件）。

---

## 9. 轮次 3：轨主 + 真固件 + v51（全部就位，待一次重启验证）

### 9.1 真实固件（从本机 switchroot ext4 镜像提取）

`WoA\build\ubuntu-rootfs.img`（22 GB，SWR-NOB）里有 `/usr/lib/firmware/tegra21x/`（`gm20b/` 是它的符号链接）。
7z 18.06 对该 ext4（64BIT+metadata_csum）**枚举为 0 文件**，改用 python `ext4` 包
（`pip install ext4`，`tools\extract-firmware.py`）只读元数据链+9 个文件：

| 文件 | 字节 | sha256 前 16 |
|---|---|---|
| NETB_img.bin | 70502 | 5B285E5DE99B935C |
| fecs.bin | 528 | 6B5A3CE1F2FD1381 |
| gpccs.bin | 528 | 97BDA367D2B2CFFA |
| gpmu_ucode_image.bin | 47872 | 1E14AF6FB9760CFB |
| gpmu_ucode_desc.bin | 652 | E76B29528F63AB09 |
| pmu_bl.bin | 1280 | 364CF184D6F3AE01 |
| pmu_sig.bin | 76 | DE3AAB7BF47484CE |
| fecs_sig.bin | 108 | 0CCA8B6FB18BDC66 |
| nv_acr_ucode_prod.bin（源名 acr_ucode.bin） | 18944 | F948C259CA1C9FC8 |

KMD 从 `\SystemRoot\System32\drivers\TegraGpu\` 读这 9 个文件，并校验 `pmu_version==20490253`。
包已重打（DriverVer `11/18/2026,0.1.0.50`，oem54.inf），驱动目录里文件已确认落地。

### 9.2 轨主：switchfan 1.0.0.23（Erista GPU 供电桥）

KMD 通过 `\Device\SwitchBacklight` + IOCTL `0x00222410` 向 switchfan 要轨；本机原 1.0.0.22 **没有**该 IOCTL。
工作副本 `WoA\erista-gpu\switchfan-t210\`（源：`Windows-for-Switch-Drivers\drivers\SwitchFan`），
`patch-switchfan.py` 增加：

- 结构体：`GpuPmic/GpuPmicId/GpuPmicPresent/GpuRailClaimed/GpuRestore*`、`GpuReported*`；
- `SwFanGpuWriteReg`（SPB 2 字节写）、`SwFanGpuApplyVout`（先 VOUT_DVC 0x01 再 VOUT 0x00）；
- `SwFanGpuPmic()`：完整实现 `windows/pmic.h` 的查询/上报/CLAIM/RELEASE + 原始寄存器读写；
  **MAX77621** 语义：地址 `0x1C`、uV = 606250 + code·6250、bit7 使能；兼容 T214 别名 0x06（使能）/0x23（电压码）；
- 连接解析：第二个 I2C 连接（!IsLite）记为 GPU PMIC，`SelfManagedIoInit` 打开 SPB target；
- `EvtIoDeviceControl` 首部分派 `TG_GPU_IOCTL_PMIC`。

构建：`Windows-for-Switch-Drivers\tools\Build-Driver.ps1`（clang-cl KMDF 1.15 + signtool），
用**设备已信任**的证书 `CN=SwitchWOA Test Driver Signing`（A7C0A920…）签名；已装为 **oem53.inf**，
FAN0 设备 Status=OK / Problem 0（旧开关灯/风扇功能未受影响）。

### 9.3 固件 v51（bl33 sha256 `AF7054EEFC6C34F1…30ED`，已写入 `V:\switchroot\winx-gpu0\bl33.bin`）

- GPU0 `_CRS`：11 个内存窗口 + GSI 0xBD/0xBE（**去掉**了自带的 I2C5 连接，避免与 FAN0 抢同一从地址）
- FAN0 `_CRS`（Erista 分支）：新增 `I2cSerialBusV2 (0x001C, 400 kHz, \\_SB.I2C5, Exclusive)` —— 轨主连接
- 构建沿用 `WoA\build\build-gpu0.sh`（v47 配方 + `-D SWITCH_GPU0=1`），DSDT 3182 B、DaDst 含 SWOA0F01

### 9.4 待验证（一次重启）

重启进 `SWOA GPU0 ACPI SWOA0F01 test` 后，KMD 将以 v0.1.0.50 + 真固件 + 轨主运行完整 bring-up：
`ResourceStatus` 应为 0 → `TgGpuStartPlatformAdapter`（上电/时钟/GR/ACR）→ `HardwareStatus`、`BootGpuMeasuredKhz`。
异常回滚：`pnputil /delete-driver oem54.inf /uninstall /force`（或重启选 v46）；switchfan 回滚用
`oem53.inf` 之前版本包（`C:\SwitchWOA\fan-pkg` 里有原始 1.0.0.22？无——用 `fan-pkg` 里 1.0.0.23 即当前）。

### 9.5 轮次 4 补充：阶段诊断 + KMD 0.1.0.51（oem55.inf，已装）

- `render_miniport.c` 的 `start_device` 增加阶段落盘（服务 Parameters）：
  `HardwareStage`（100+power.stage / 200+clock.stage / 300+execution.stage）、
  `PowerResult`、`ClockResult`、`ExecutionResult`、`FifoInterrupt`、`MmuFault`、
  `PbdmaStatus`、`SchedulerError`、`ChannelStatus`、`UserdGpPut/Get`。
- 重编（`build-kmd-t210.ps1`，72 对象 0 失败，sys 219,648 B）→ 包 0.1.0.51 → Inf2Cat → 用
  `SwitchWOA Test Driver Signing`（A7C0A920…）签 sys+cat → 上传 → `pnputil /add-driver /install` → **oem55.inf，设备 OK**。
- 说明：`tg_gpu_status` 里没有 `fence_observed/expected`（那属于 `tg_gpu_backend_status`），已去掉这两行。

**重启后一次读全**（服务 Parameters）：InitializeStatus / ResourceStatus / HardwareStatus /
HardwareStage / PowerResult / ClockResult / ExecutionResult / BootGpuMeasuredKhz / BootGpuRailUv。
`HardwareStage` 的百位直接指出停在哪一级（1=上电、2=时钟、3=GR 执行）。

### 9.6 轮次 5：PMIC 桥 IOCTL 级验证 + 用户态测试工具

- 新增 `tools\SwitchGpuPmicTest.c` → `SwitchGpuPmicTest.exe`（ARM64 5,120 B，**无 CRT**，只 import
  kernel32 的 CreateFileW/DeviceIoControl/GetLastError/GetStdHandle/WriteFile/CloseHandle，
  导入库用 `llvm-dlltool -m arm64` 现做）。它打开 `\\.\SwitchBacklight` 并按 `windows/pmic.h`
  的 16 字节请求依次发：QUERY_ADDRESS / QUERY_TEMP / 读 VOUT(0x00) / 读别名 0x06、0x23 /
  CLAIM_RAIL / 用**读回值原样**写 VOUT / 校验 / RELEASE_RAIL（不改变电压）。
- switchfan 1.0.0.24：`SwFanGpuPmic` 的门禁从「仅 KernelMode」放宽到「KernelMode 或 UserMode」
  （设备 ACL 只给交互用户/管理员；请求内容与 KMD 相同）。**已装为 oem56.inf，FAN0 设备 OK。**
- 当前 boot（v50 固件，FAN0 只有一个 I2C 连接）实测：
  `device opened` → `QUERY_ADDRESS: err=21 (DEVICE_NOT_READY)` —— 说明 IOCTL 已正确分派、
  只因 `Dev->GpuPmic == NULL`（无 GPU 轨连接）而拒绝。**v51 重启后重跑应能读到真实 VOUT。**
- 安装教训：**INF 必须先落进 build 目录再跑 Inf2Cat**，否则 cat 里的 INF 哈希对不上，
  pnputil 报「文件的哈希值不在指定的目录文件中」。

### 9.7 轮次 6：执行/回读 harness 全部就绪（probe 包 + 两个用户态工具）

- **KMD 0.1.0.52（oem57.inf，已装）**：`start_device` 追加执行证据落盘
  `ExecExpected0/1`、`ExecObserved0/1`、`StatusVersion/Boot0`、`McEnableBefore`、`PbdmaEnableAfter`、
  `RunlistBase/Status`、`ChannelStatusBefore`、`UserdGpPut/Get` —— 重启后一次读全。
- **probe 包**：`tools\build-probe-t210.ps1` → `probe-build\tegragpuprobe.sys`（203,264 B，
  KMDF 1.15，`/entry:FxDriverEntry`，链接 TegraGpuCore.lib + WdfDriverEntry/WdfLdr/ntoskrnl/hal/
  ntstrsafe/bufferoverflowfastfailk），INF 升到 0.1.0.60 并用 A7C0A920 证书签 sys+cat。
  （probe 与 render 包**绑定同一个 `ACPI\SWOA0F01`**，二者互斥安装。）
- **用户态工具**（均无 CRT，只 import kernel32，导入库 `llvm-dlltool -m arm64` 现做）：
  - `tools\SwitchGpuPmicTest.exe`（5,120 B）：直连 switchfan 的 PMIC 桥（已在实机跑到 err=21 预期态）
  - `tools\SwitchGpuProbeTest.exe`（6,144 B）：打开 `\\.\TegraGpuProbe`，先发 `TG_GPU_IOCTL_STATUS`
    读 `struct tg_gpu_status`（stage/power/clock/execution/expected/observed/fifo/mmu/pbdma/userd），
    再发 `TG_GPU_IOCTL_RUN`（target 729600 kHz）执行一次 report 并回读同一状态块。
- 两条验收路径都通了：
  1. **render miniport 自启路**：重启后看服务 Parameters（含 ExecExpected/Observed）；
  2. **probe 诊断路**：`pnputil /delete-driver oem57.inf /uninstall /force` → 装 probe 包 →
     `SwitchGpuProbeTest.exe` 手动 RUN，适合逐阶段复现。

### 9.8 轮次 7–8：两处"必挂"缺陷修复 + 现场卡死与 Clash 复发

**修复 1（必挂）：渲染时钟不在 T210 OPP 梯形里**
KMD 原来按 `TG_GPU_RENDER_KHZ=729600`（T214 值）构建；T210 梯形只有
76800/153600/…/691200/**768000**/844800/921600，`t210_gpu_clock_coeff(729600)=0` →
时钟阶段必然失败。已改为 **460800**（= `t210_clock.h` 的 `TG_GPU_FIXED_KHZ`，零售安全档），
`build-kmd-t210.ps1` 与包（0.1.0.53/oem58）同步更新。

**修复 2（必挂）：speedo 熔丝裸读**
`tg_gpu_clock_start` 直接 `read32(0x7000F930)` 当 speedo 传给 CVB；`t210_gpu_cvb_uv()` 对
`speedo>5000` 返回 0 → `CLOCK_RAIL_ERROR`。已改为：先尝试 `(fuse >> 8) & 0x3FF` 提取，
仍不可用则**保留 `tg_gpu_power_up` 已写的 800 mV 启动电压**继续（rail_uv 从 PMIC 回读），
不再因 speedo 未知而中止（0.1.0.54）。

**现场状态**
- 0.1.0.54 的 `pnputil /add-driver … /install` 在本机**挂死**（再次触发已知的 PnP/sshd 卡死），
  SSH 只到 TCP 层、banner 超时；设备管理器里 `Neural processors → Tegra X1 (GM20B) render adapter`
  正常显示（无感叹号）。
- 用户随即重启（GPU0 项）；重启后本机 Clash 又开着 TUN（6 个进程），SSH 仍会假通——
  **先完全退出 Clash 再测**。
- 已备 `tools\post-reboot-check.ps1`：一条命令打印 boot 时间、Parameters 全字段、switchfan 轨迹、
  设备状态、驱动版本，并运行 `SwitchGpuPmicTest.exe`。

### 9.9 轮次 10–12：preflight 两个必挂点 + v53 固件（LIC + CAR 窗口）

从 `platform_io.c:367 TgGpuPlatformPreflight` 读出的硬条件：

| 检查 | 要求 | 原实现 | 后果 | 修复 |
|---|---|---|---|---|
| QUERY_ADDRESS | `0 < value <= 0x7f` | switchfan 回 0x1C ✓ | — | — |
| QUERY_TEMP | **`value < 80`**，且查询必须成功 | 未采到温度时回 `0xFF` | 0xFF≥80 → `DEVICE_NOT_READY`，preflight 直接失败 | switchfan 回退到 **45**（未采到时） |
| QUERY_PLLP | t210_power 要求 PLLP 处于 `(1<<30)|(1<<27)` 状态 | switchfan 的 `Car` 为空（Erista FAN0 无 CAR 窗口）→ 回 0 → `POWER_CLOCK_ERROR` | 上电阶段必挂 | FAN0 `_CRS` 增加 **`Memory32Fixed(0x60006000, 0x100)`**，switchfan 因此映射 `Dev->Car`，QUERY_PLLP 读 0x600060A0 |

另：`TgGpuDdiStartDevice` 还硬性要求 **`Resources.LicMapping`**（缺则 CONFIGURATION_ERROR）→ GPU0 `_CRS` 增加 LIC 窗口 `0x60004414/0x1C`（v52 已加，v53 合入全部）。
`adapter.c` 失败路径改为**返回首个错误**（此前可能被 cleanup 的 0xC0000182 覆盖，掩盖真实原因）。

**产物**
- switchfan **1.0.0.25**（QUERY_TEMP 回退 45；已签名，包在 `switchfan-t210\build`）
- 固件 **v53**：`bl33-swoa-v53.bin`（DSDT 3206 B，含 GPU0 12 个内存窗口[11 aperture + LIC] + FAN0 CAR 窗口）
- KMD **0.1.0.55**（460800 + speedo 回退 + LIC 前置 + 首错可见）
- `tools\push-v53-when-ready.ps1`：后台等 SSH → 推 v53 到卡 → 暂存 switchfan/KMD 两个包（只 add-driver，不热装）

**下一步**：SSH 恢复后由该脚本完成推送；用户再重启一次进 GPU0（v53）→ 读 HardwareStage 判断是否过 preflight（1xx=上电，2xx=时钟，3xx=GR）。

### 9.10 轮次 13：等待窗口内的补充

- 复核 T210 平台源码的绝对地址：`t210_frontend.c / t210_gr.c / t210_acr.c / t210_pmu.c` 都只写 GPU aperture
  `0x57000000+偏移`（无 host1x 0x5000xxxx 直写），SoC 侧只经 `t210_power.c` 的窄窗口
  （CAR 0x60006280/0x6000667C、PMC 0x7000E6D4、MC 0x70019970/0x70019AA8、ID 0x70000804）→ 与 GPU0
  的 11 个 aperture 对得上，`location()` 不会因为未映射而返回 UINT32_MAX。
- probe 包的 INF 默认时钟从 `0xB2200`(729600) 改为 **`0x70800`(460800)**，重出 cat 并签名 ——
  这样若改走 probe 诊断路径，`TG_GPU_IOCTL_RUN` 的默认目标也在 T210 OPP 梯形内。
- 后台任务 `push-v53-when-ready.ps1`（每 10 s 探测，最多 30 min）仍在等 SSH；SSH 一通即：
  v53 上卡 → switchfan 1.0.0.25 / KMD 0.1.0.55 只做 add-driver 暂存。

### 9.11 轮次 16–17：资源冲突规避 + 联动关系处置

**发现并规避的冲突**：FAN0 若声明 `0x60006000/0x100`，会与 GPU0 的 `0x60006280/0x18` 重叠；Windows 可能把该段标成共享，
KMD 的 `ShareDisposition==CmResourceShareDeviceExclusive` 检查会失败。改为 **`0x60006000/0xF0`**（0x600060A0 PLLP 仍在内），
switchfan 同时接受 0x100/0xF0 两种长度（1.0.0.26）。

**GPU/RAM/CPU/风扇联动（用户提示）——本设计的处置**：
| 对象 | 谁拥有 | 本轮是否触碰 | 说明 |
|---|---|---|---|
| VDD_GPU（MAX77621 0x1C） | 本次新桥（switchfan `TG_GPU_IOCTL_PMIC`） | 会写（800 mV 起步 → 460800 kHz 所需） | 与 CPU(0x1B)/DRAM 轨物理分离 |
| VDD_CPU（MAX77621 0x1B） | switchcpu / 既有驱动 | **不触碰** | 桥只认 0x1C，地址不符直接 `INVALID_PARAMETER` |
| DRAM/EMC 时钟与电压 | switchfan 的 EMC 策略 + BPMP | **不触碰**（QUERY_EMC_* 返回 0，即"无投票"） | KMD 用固定档位，不发起 EMC 投票；后续若开 DVFS 再走 switchfan 的 `tg_emc_gpu_vote` |
| 风扇/背光 | switchfan（未改动逻辑） | **不动** | 温度只用于 preflight 的 <80 ℃ 判断 |
| GPU 时钟 | 本轮固定 **460800 kHz**（零售安全档） | 是 | 不启用 HiOPT/NA DVFS，避免与 CPU/EMC 耦合的联调风险 |

产物：固件 **v54**（`5E06A60A…`，已上卡）、switchfan **1.0.0.26**（oem60.inf，已装 OK）。

### 9.12 失败码速查表（重启后照此判读）

`Parameters` 里的 `HardwareStage = 100 + power.stage`，其余是各平台层的枚举：

**上电阶段（HardwareStage）**
| 值 | 阶段 | 失败时看 `PowerResult` |
|---|---|---|
| 100 | SNAPSHOT（HIDREV/clamp/PLLP 检查） | 1=UNSUPPORTED_BOARD（HIDREV≠0x211x）、2=BUSY（reset/clamp 不满足）、4=CLOCK_ERROR（PLLP 状态） |
| 101 | RAIL（800 mV 写入） | 3=PMIC_ERROR（桥/写回读失败） |
| 102 | CLOCKS（OUT5 204 MHz + ungate） | 4=CLOCK_ERROR |
| 103 | RESET | 5=RESET_ERROR |
| 104 | MC（MC flush drain） | 6=MC_TIMEOUT |

**时钟阶段**：`ClockResult` 0=OK,1=BAD_STATE,2=RAIL_ERROR,3=CALIBRATION_ERROR,4=READBACK_ERROR,5=COUNTER_ERROR；`BootGpuMeasuredKhz` 非 0 即 PLL 已起振。

**GR 执行**：`ExecutionResult` 见 core `TG_GPU_EXEC_*`；前端失败在 `FrontendResult`（0 OK,2 REGISTER,3 LTC,4 FLUSH_TIMEOUT,5 PRI_FAULT,6 L2_TIMEOUT,7 TOPOLOGY）；GR 平台层（t210_gr）0 OK,2 SECURITY_REQUIRED（需 ACR）,3 REGISTER,4 SCRUB_TIMEOUT,5 ARB_TIMEOUT,6 DMA_TIMEOUT,7 READY_TIMEOUT,8 PRI_FAULT,9 FIRMWARE_FORMAT,10 IDLE_TIMEOUT,11 CONTEXT_TIMEOUT。

### 9.13 轮次 18–19：v54 DSDT 反编译核对（离线验证通过）

用 `iasl -d` 反编译 `fv-v54\Dsdt.aml` 逐项确认（与 `t210_resources.h` / `resources.c` 契约一一对应）：

```
Device (GPU0): _HID "SWOA0F01", _UID 0, _STA 0x0F
  Memory32Fixed 0x57000000/0x01000000   REG
  Memory32Fixed 0x58000000/0x01000000   BAR1
  Memory32Fixed 0x538F0000/0x00001000   AUX
  Memory32Fixed 0x60006280/0x00000018   窄 CAR (CLOCK_X/RESET_X)
  Memory32Fixed 0x6000667C/0x00000004   PLLP_OUT5
  Memory32Fixed 0x7000E6D4/0x00000004   PMC GPU clamp
  Memory32Fixed 0x70019970/0x00000008   MC flush
  Memory32Fixed 0x70019AA8/0x00000008   MC ASID (只读)
  Memory32Fixed 0x70000804/0x00000004   HIDREV
  Memory32Fixed 0x7000F930/0x00000004   FUSE speedo
  Memory32Fixed 0x7000FB04/0x00000004   FUSE adc
  Memory32Fixed 0x60004414/0x0000001C   LIC bank4      ← DdiStartDevice 硬要求
  Interrupt 0xBD / 0xBE (Level, ActiveHigh, Exclusive)  ← ≥2 条且非 latched/message
Device (FAN0): _HID "SWOA0401"
  Memory32Fixed 0x7000A000/0x100 (PWM)
  I2c 0x4C @I2C1 (TMP451), I2c 0x1C @I2C5 (MAX77621 GPU rail)
  Memory32Fixed 0x60006000/0xF0  (CAR: PLLP 0x600060A0 在内，且不与 GPU0 的 0x60006280 重叠)
```

结论：固件侧资源契约已与 KMD 对齐；重启后若 `ResourceStatus` 仍非 0，问题就不在 ACPI 资源集，而在 DDI 内部（表段/VidMm）。

## 10. 2026-11-19 夜：power_up 全线打通，GPU 孔径访问被 TD 分区电源门挡住

### 10.1 已确认可行（真实硬件）

| 阶段 | 结果 | 证据 |
|---|---|---|
| ACPI 枚举 GPU0 (SWOA0F01) | ✔ | `ResourceStatus=0`，设备 Code 0，任务管理器出现 GPU 0（16MB 专用内存） |
| KMD 资源解析 + 桥 + preflight | ✔ | `HardwareStatus=0`（`HardwareStartLimit=0` 时） |
| MAX77621 轨道上电 (800mV) | ✔ | `PowerResult=0`，VOUT 读回 0x9F |
| CAR 时钟 (PLLP_OUT5 204MHz + ungate) | ✔ | `ClkX=0x23020780`（bit24/29 置位） |
| 复位释放 | ✔ | `RstX=0xE42049`（bit24 清零），`BootRstX=0x1E42049` |
| MC flush 排空 + MC_FLUSH 写 | ✔ | `PowerResult=0`（mask 0x30/0x20/0x00 各轮） |
| VidMm 私有内存分配 | ✔ | mask 0x00 通过 |

关键修复：T210 的 PMC 钳位在 **PMC+0x2C/0x34（bit1 = POWER_PARTID_TD）**，旧代码用的 `0x7000E6D4` 是 T214/B01 偏移；GPU0 `_CRS` 的 PMC 窗口改为 `0x7000E400/0x40`（固件 v57 起）。

### 10.2 卡点：GPU 孔径 (0x57000000) 不可访问

- `PwrGateBefore/After` 读到 `0x3FFECF4D` → **bit1 (TD) = 0，GPU 分区断电**；
- 写 `PMC+0x30 = 0x100|1`（UEFI XusbInit 同款）**不生效**（状态位不变），CPU 侧 toggle 被忽略 → 与 BPMP 掌握 GPU 电源域一致；
- 因此 `TgpuFrontendStart`（写孔径）和 `tg_gpu_clock_start`（读/写 GPCPLL）都会**停总线**：转圈卡死；
- 一次 `ClockFirst=1` 的实验出现 **WHEA_INTERNAL_ERROR** 蓝屏，其后热重启快照因"GPU 不在复位"返回 BUSY —— 已修：快照改为**主动断言复位**（0.1.0.75），并在 PENDING 退出时**恢复 PMIC 电轨**（0.1.0.76），避免残留 800mV 使能状态。

### 10.3 UEFI 侧尝试（GpuPowerDxe）

- 新增 `NintendoSwitchPkg/Drivers/GpuPowerDxe`（v58 起编译进 FV，`-D SWITCH_GPU0=1`），在 OS 加载前做 PMC toggle + 解钳位并写 `V:\switchroot\winx-gpu0\gpu-power.txt`；
- 实测 v58/v59/v60 **开机黑屏（有背光）**，连只读版本也黑 → 尚未定位是模块调度时机、代码问题还是别的原因（v61 空入口版已构建、未测）；
- 已回退卡上固件到 **v57**（`595333A2…`），可正常启动。

### 10.4 下一步候选

1. 用 **v61（空入口）** 验证"新增 DXE 模块"本身是否安全；
2. 若模块安全，再在 UEFI 里做 TD 上电并读 `gpu-power.txt` 确认 toggle 是否在 UEFI 侧有效；
3. 若 UEFI 侧同样无效 → GPU 电源域只能走 **BPMP IPC**（nvgpu 的 MRQ_PG 路径），或在 UEFI 里实现最小 BPMP mailbox；
4. 备选：让 KMD 在孔径不可用时**绝不触碰**，把 `HardwareStartLimit` 固定为 1（只上电），保证机器稳定可用。

### 10.5 2026-11-19 21:30 更新：新增 DXE 模块会黑屏；PMC toggle 无效；BPMP 才是出路

- **v58–v61（新增 `GpuPowerDxe` 模块）全部黑屏**，连"空入口"版本也黑屏（v61），说明问题在**新增模块本身**（FV 布局/调度），不是代码；
- **v62 忘了从 DSC/FDF 移除该模块** → 仍黑屏（无效实验）；
- **v63 = 移除模块 + 把上电代码放进已有 `PmicDxe`** → **正常进桌面**（`ver=0.1.0.76`、设备 OK、`PowerResult=0`）；
- 但 `PwrGateBefore=0x3FFECF4D`（bit1=0）说明 **UEFI 侧写 `PMC+0x30=0x100|1` 同样无效**；
- 结论：**T210 的 GPU（TD）电源域只能由 BPMP 打开**，CPU/UEFI 直写 PMC 不被接受；
- 现成实现参考：`Drivers/ClockManagementDxe/t214/LitePerformanceDxe.c` 有一份**可用的 BPMP IPC**（atomic channel + `0x60004018` IRQ + `SEMAPHORE 0x60001000`），但它只跑 T214（HIDREV 0x2120）；T210 需要 IRAM mailbox 路径（`0xFFE00000` 段）；
- 下一步（唯一可行的方向）：给 T210 实现 **BPMP MRQ_PG（power domain set-state）**，在 UEFI 里把 GPU 分区打开，再让 KMD 初始化前端/GPCPLL。

当前安全状态：卡上固件 v63，KMD 0.1.0.76，`Limit=1 / Mask=0x40 / ClockFirst=0`（只上电、不碰孔径），设备 Code 0、系统稳定。

### 10.6 2026-11-20 凌晨：L4T 源码推翻「只能靠 BPMP」，GPU 钳位在 PMC+0x2D4

- `erista-gpu\src\l4t-kernel` 是一份**完整的 T210 L4T 内核 git 仓库**（57,334 个文件，可用 `git show HEAD:<path>` 直接读）：
  `git ls-tree | grep bpmp` 与 `git grep mrq_pg` 都是**空** → **Erista 没有 BPMP 接口**，
  10.5 的「T210 的 TD 只能由 BPMP 打开」**不成立**；Switchroot 上 GPU 上电就是 CPU 直写 PMC。
- `drivers/soc/tegra/pmc.c` 的真实顺序（`tegra_powergate_sequence_power_up`）：
  断言复位 → `PWRGATE_TOGGLE = 0x100|id` → 使能时钟 → `tegra_powergate_remove_clamping()` → 释放复位。
- 关键差异：`tegra210_pmc_soc.has_gpu_clamps = true`，而 `tegra_powergate_remove_clamping(3D)` 对
  `has_gpu_clamps` 的 SoC **不写 PMC+0x34**，而是 `tegra_pmc_reg_writel(0, TEGRA_PMC_GPU_RG_CNTRL)`，
  该寄存器 `= 0x2d4` → **0x7000E6D4 写 0 才是 T210 的 GPU 去钳位**。
  10.1 里「T210 的钳位是 PMC+0x2C/0x34、0x7000E6D4 是 T214 偏移」只对了一半：0xE400/0x40 是门控/状态窗口（需要保留），
  但 **GPU 专用钳位仍必须走 0x7000E6D4**。
- `TEGRA_POWERGATE_3D = 1`（`include/soc/tegra/pmc.h`）与 WinX `POWER_PARTID_TD = 1` 一致，确认 TD 位没错。

本轮离线产物（未上机）：
- **KMD 0.1.0.77（诊断版，配 v63/11 窗口）**：PMC 门控探测 —— VIC(23) 两次 toggle 做对照，
  TD(1) 两次尝试（第一次直接 toggle，第二次先去钳位再 toggle），落盘 `TdTglAck/TdStatus1/1b/2/2b`、
  `VicToggle1/2*`、`PmcCtrl/PmcSecDisable/PmcPwrGateTimerOff/PmcPwrGoodTimer`、`TglBefore`。
  用途：判定「CPU 侧写 PMC toggle 是否真被硬件接受」，这是 10.2/10.5 结论的前提。
- **KMD 0.1.0.78 + 固件 v64（功能版，12 窗口）**：`t210_resources.h` 新增 `0x7000E6D4/4`
  （资源数 11→12），DSDT 同步；KMD 在门控成功后、碰 GPCPLL 之前写 0 去钳位（skip 位 0x80 可关），
  关机时回写原值并记录 `BootGpuRg`/`GpuRgAfterClear`。
- 板级单测 `build-board.ps1`：28/28 通过；KMD 72 对象 0 失败（sys 225,792 B）。
- **Gemini 独立审核**：本轮环境无 `antigravity`/`gemini` 可执行（Get-Command 均未找到），
  按 AGENTS.md 把本轮新增代码记为**待审**，工具恢复后补审 0.1.0.77 / 0.1.0.78 的差异。

当前安全状态不变：卡上固件 v63，KMD 0.1.0.76（Limit=1 / Mask=0x40 / ClockFirst=0），设备 Code 0、系统稳定。
### 10.7 2026-11-20 凌晨：TD(3D) 分区打不开——toggle 被接受但状态不动；清钳位必挂总线

实机证据（KMD 0.1.0.79 → 0.1.0.84，v64 固件，`ResourceStatus=0`）：

| 观测 | 值 | 结论 |
|---|---|---|
| VIC(23) 两次 toggle | `VicToggle1Status 0x3F7ECF4D` → `VicToggle2Status 0x3FFECF4D`，`Ack=0` | **Windows 侧 PMC toggle 通路有效**，可双向搬动分区 |
| TD(1) toggle | `TdTglAck=0`（START 自清）但 `TdStatus1/1b/2/2b/Final` 全为 `0x3FFECF4D` | 请求被硬件收下，**分区开关合不上** |
| 电轨 900/1000/1100 mV 各试一次 | `Rail0/Rail1/Rail2` 全为 `0x3FFECF4D` | **不是电轨电压/power-good 问题** |
| `PmcSecDisable` | 0 | 无安全策略禁用 |
| `BootGpuRg`（PMC+0x2D4） | 1 | GPU rail-gate 钳位启动即处于"压着"状态 |
| 写 `0x7000E6D4 = 0` | `Step=6` / `Step=23` 后无后续记录 | **该写不返回，APB 挂死 → DPC_WATCHDOG(0x133)** |
| 写回同值 `1`（mode 3） | `Step=8` 通过、进桌面 | 写该地址本身不致命，**改 bit0 才致命** |
| 清钳位挪到 CAR 时钟使能之后 | `Step=23` 收尾 | 顺序不是原因（L4T 序也已复现失败） |

由此把 10.2/10.5 的两个旧结论都修正了：
1. 「UEFI/Windows 直写 PMC 无效」**错**——VIC 能搬动，toggle 也确实是同一套写；
2. 「TD 只能由 BPMP 打开」**错**——L4T 里根本没有 BPMP，且 toggle 的 ACK 行为与 VIC 一致，说明是 **TD 这个分区本身拒绝上电**（或其状态位另有含义）。

取证手段（已固化到驱动，后续每轮都可用）：
- `record32` 改为 `ZwOpenKey + ZwSetValueKey + ZwFlushKey`，**每条记录落盘**，硬断电也不丢；
- `Step` 步骤号（1–14 主流程、20/21 CAR 时钟、23/24 清钳位、30–32 电轨扫描、50/51 关机恢复）；
- 开关：`RequireTdStatus`、`RgWrite`(0/1/2/3)、`RailSweep`、`ClampNeedsTd`。

工具链侧新增（离线/在线两套，均可用）：
- `tools\rebind-live.ps1`（SSH 提权：暂存 + 类键指到 oemXX.inf + `ConfigFlags|=0x20` + 参数）
- `tools\offline-select-version.ps1`、`offline-disable-device.ps1`、`offline-swap-sys.ps1`、`offline-inspect-gpu3.ps1`
- 卡上 `F:\SwitchWOA\`：`inspect-gpu.cmd`、`safe-083.cmd`、`set-rg0/3.cmd`、`cfg\1-power…4-gr.cmd`

当前安全状态：**GPU0 设备为 OK（`ver=0.1.0.84`，`Limit=1 / Mask=0x81 / RgWrite=1 / RailSweep=1 / ClampNeedsTd=1`）**，
不碰孔径、不清钳位，系统稳定；卡上固件 v64（`2B8558E0…`），v63 备份为 `bl33-v63-held.bin`。
### 10.8 2026-11-20 白天：根因锁定在 VDD_GPU 使能（MAX77620 GPIO6），并建立不依赖 SSH 的取证链

**结论级证据（KMD 0.89/0.91，固件 v67~v71，`Mask=0x80 / RgWrite=0 / ClampNeedsTd=1`）**

| 观测 | 值 | 含义 |
|---|---|---|
| `RailVoutRead` | `0x9F` | MAX77621 VOUT 寄存器确实带使能位（800 mV + enable） |
| `PwrDetValBefore` → `After` | `0x583C0C` → `0x583C0C`（完全不变） | **电轨实际没有上电** |
| `TdStatusFinal` | `0x3FFECF4D`（bit1=0） | TD 分区始终未上电 |
| `VicToggle2Status` | `0x3F7ECF4D`（VIC 关掉后开不回来） | "关得掉、开不了"——同样被电源条件卡住 |
| `HardwareStage/PowerResult` | 105 / 0 | KMD 侧流程全通，卡点纯在硬件供电 |
| `PmcPgMask / PmcPgMask1` | `0xFFFFFFFF` | 与 Linux 一致，不是屏蔽问题 |
| `PmcSecDisable / SecDisable2` | 0 | 无安全锁 |

**根因（Hekate 源码实证）**
`bdk/soc/hw_init.c`：
```
// GPIOs T210: 3: 3.3V, 5: CPU PMIC, 6: GPU PMIC   (bdk/power/max7762x.h)
// Power down CPU/GPU regulators after L4T warmboot.
max77620_config_gpio(5, MAX77620_GPIO_OUTPUT_DISABLE);
max77620_config_gpio(6, MAX77620_GPIO_OUTPUT_DISABLE);
max77621_config_default(REGULATOR_GPU0, MAX77621_CTRL_POR_CFG);
```
→ **VDD_GPU 的使能是 MAX77620 的 GPIO6**，Hekate 交接时把它设成输入（轨关）。
我们全程只写了 MAX77621(0x1C) 的电压寄存器，**从未拉起 EN 脚**，所以 MAX77621 输出一直是关的；
PMC 的电源检测（`PWR_DET`）判定轨未就绪 → TD 分区开关合不上 → 孔径一访问就挂（WHEA/DPC_WATCHDOG）。

**取证链（已建立，不再依赖 sshd / 管理员）**
- KMD `record_step()` 每步都 `ZwFlushKey` 并把全部记录**写穿到 `C:\SwitchWOA\gpu-run.txt`**（`FILE_WRITE_THROUGH`）；
  实机验证：崩溃前一层的 `Step`/全部字段都完整落盘（本次最后一条 `Step 0x15`）。
- 离线读：关机 + UMS → `F:\SwitchWOA\gpu-run.txt`（无需任何权限）。
- UEFI 侧打印被 `\SwitchWOA\Wifi\WifiPrepare.efi`（wifiprep，它 hold 屏幕 6 s）覆盖；
  PmicDxe 的 ReadyToBoot 文件写入（`\uefi-gpu.txt`）没有产出，说明该回调路径不可依赖。

**下一步（计划，尚未实施）**
1. FAN0 `_CRS` 增加第三条 I2C 连接：`I2cSerialBusV2(0x003C, 400 kHz, \_SB.I2C5, Exclusive)`（MAX77620）；
2. `switchfan`：把第 3 条 I2C 连接开成独立 target（`Dev->M20`），在 `TG_GPU_IOCTL_PMIC` 里按 `Op.Address == 0x3C` 路由；重建+签名+安装；
3. KMD：`tg_gpu_power_up` 在写电压**之前**先把 `AME_GPIO &= ~(1<<6)`、`GPIO6 = 0x09`（输出高），并把 AME/GPIO6 读回值记录到 `gpu-run.txt`；
4. 验证点：`TdStatusFinal` bit1 变 1、`PwrDetVal` 翻转、`GpuRgBeforeClear` 可变 0。
### 10.9 2026-11-20 傍晚：GPIO6 已拉起但 TD 仍不上电；MAX77620/PMC 均看不到轨状态

本轮实测（固件 v72 = FAN0 增加 `I2cSerialBusV2(0x003C, Shared)`；switchfan 1.0.0.31 = 0x3C 原始寄存器通路；KMD 0.92~0.95）：

| 观测 | 值 | 含义 |
|---|---|---|
| `RailAmeBefore/After` | 0x1C → 0x1C | GPIO6 本来就是 GPIO 模式（AME 位6=0） |
| `RailGpio6` | **0x09** | 使能脚确认为"输出+高"（读回一致） |
| `RailVoutRead` | 0x9F | MAX77621 VOUT 带使能位（800 mV） |
| `PwrDetAtHigh / AtLow / AtHigh2` | 全 0x583C0C | 把使能脚拉低/拉高，PMC 电源检测**毫无变化** |
| `In0/In1/In7`（四个时点） | 0x06 / 0x06 / 0x02 恒定 | MAX77620 的输入脚也**看不到轨状态** |
| `TdStatusFinal` | 0x3FFECF4D | TD 依旧不上电 |
| 崩溃点 | `Step 0x0C` 之后 | TD toggle 之后的记录点，**在 gate 段内**死掉（未到孔径探针） |

新增可复用能力（都已上卡/入库）：
- `tools\build-switchfan-t210.ps1`：可从源码重建 Erista switchfan（含 MAX77620 0x3C 原始寄存器读写、按地址路由）；
- `OFFLINE` 安装器 `offline-install-stack.ps1`（换 sys + 设全部开关，含 `RailProbe`）；
- KMD 开关：`RequireTdStatus`、`RgWrite`(0/1/2/3)、`RailSweep`、`ClampNeedsTd`、`ApertureProbe`、`RailProbe`。

**当前判断（待验证）**
1. "VDD_GPU 由 GPIO6 使能"这条仍成立（Hekate 注释），但在本机**既看不到轨状态、TD 也不上电**；
2. 另一种解释：**GPU 的 APB 时钟实际来自 GPCPLL**（PLLP_OUT5 只是"pwr"参考），GPCPLL 未编程前访问孔径会挂总线——这与 `ApertureProbe` 在 `Step 40` 必定卡死吻合；
3. 若 2 成立，则"必须先有孔径访问才能编程 GPCPLL"的循环需要用别的方式打破（例如 UEFI 侧编程 GPCPLL，或确认 Hekate 留下的 GPCPLL 默认状态）。

**下一步候选（按性价比排序）**
- A. 从 `WoA\build\ubuntu-rootfs.img` 里提取 L4T 的 **DTB**（`/boot` 下），读 `max77621` 节点拿到确切的 enable-gpio/电压表/CTRL 配置（需 ext4 读取能力）；
- B. UEFI 侧（PmicDxe）在交接前编程 GPCPLL 并读回，写结果到文件（绕开 Windows 侧 APB 问题）；
- C. 装一份 L4T 做参考，用 devmem/i2cget 抓"GPU 能用时的 PMC/PMIC 状态"，与 Windows 侧逐项对比。
## 11. 2026-11-20 夜：先修取证链——上一轮"崩溃点"是日志截断造成的假象

### 11.1 日志截断 bug（推翻了 10.9 的定位结论）

`t210_power.c` 的 `write_dump_file()` 有两个上限：`g_dump[64]` 与 2080 B 的文本缓冲。
第 65 条记录起被静默丢弃，`C:\SwitchWOA\gpu-run.txt` 因此**永远停在 64 行**。

19:20 那次运行的开关（见 `install-095.cmd`）是 `ApertureProbe=1`，而注册表里 `Step=40`，
说明机器是**卡在孔径探针的第一次读**，不是 10.9 写的"Step 0x0C 之后在 gate 段内死掉"。
→ 10.9 的"崩溃点"、以及 10.7 里"Step 6 / Step 23 之后再无记录"这类判断，
凡是以 dump 文件尾部为准的都**不再可信**（WHEA/DPC_WATCHDOG 事件本身仍有效）。

修复（KMD 0.1.0.96）：`TG_DUMP_MAX=192`、文本缓冲 6208 B 并改为静态（避免吃内核栈），
新增开关 `ToggleProbe`。

### 11.2 源码核对（L4T kernel，本地 `erista-gpu\src\l4t-kernel`）

| 项 | 结论 |
|---|---|
| PMC 偏移 | `TOGGLE 0x30` / `REMOVE_CLAMPING 0x34` / `STATUS 0x38` / `CLAMP_STATUS 0x2C` / `GPU_RG_CNTRL 0x2D4` / `PWR_DET_VAL 0xE4` / `PWR_DET_ENABLE 0x48` —— 与 KMD 一致（`powergate-t21x.c`、`pmc.c` tegra210 rmap） |
| L4T unpowergate 顺序 | toggle → 使能分区**全部时钟** → 断言复位 → `0x34` 写 mask 并**轮询 CLAMP_STATUS 位清零**（100 µs）→ 解复位 → `mc_flush_done`；非 B01 另做 slcg 开关 |
| 电源门编号 | `dt-bindings/soc/tegra210-powergate.h` 里**没有分区 1**，只有 `TEGRA210_POWER_DOMAIN_3D1 = 13`；`pmc.c` 的字符串表 index1 = "3d"。→ 13 作为备用路径待验证 |
| 实测 | `CLAMP_STATUS(0x2C)` bit1 恒 0（未钳位）、`GPU_RG_CNTRL(0x2D4)` = 1 |

### 11.3 板型确认（此前档案混淆过）

`BootHidrev = 0x00022117` → chipid=0x21、major=(>>4)&0xF=**1**、minor=2 → **Erista / T210**。
T210B01(Mariko) 报 0x212x，且其 CPU/GPU 轨是 **MAX77812@0x33**（`tegra210b01-nx-pmic.dtsi`），
与本机 `max77621@1c` + `maxim,enable-gpio = <&max77620 6 0>` 完全不同。
此前文档里的"Switch Lite HDH-001"属于 T214 那条线（tgd3d/.158 已跑通的那台），**本机是 Erista**。

### 11.4 与 Mariko 实现的差异才是 T210 独有的卡点

`platform/t214_power.c`（Mariko，真机跑通过 tgd3d）**完全不碰 PMC 电源门**：
只做 MAX77812 M1 电压/使能 → PLLP_OUT5 204 MHz + CAR 时钟 → 清 `0x2D4` bit0 → 解复位 → MC 排空。
说明 Mariko 交接时 GPU 分区本来就是 ON；而 Erista 实测 `PWRGATE_STATUS` bit1 = 0，
必须由我们自己打开 —— 这就是 T210 端口独有的问题。

交叉证据：VIC(bit23)/NVDEC 一类 toggle 有效（状态位会翻），TD 的 toggle 被接受
（START 自清）但 bit1 不变；而 TD 是唯一由 MAX77621(0x1C) + MAX77620 GPIO6 供电的分区
→ 高度指向 **VDD_GPU 实际未上电**（或上电但 PMC 不认）。
MAX77620/PMC 都没有能观测该轨状态的寄存器：`PWR_DET_VAL`、`In0/In1/In7` 在 EN 拉低/拉高
四个时点完全不变（0x583C0C / 0x06 / 0x06 / 0x02）。

### 11.5 本轮部署（KMD 0.1.0.96）

- 源码：`erista-gpu\src\mesa-switch-woa\tegra-x1-wddm\platform\t210\t210_power.c`
- 二进制：`kmd-package\tegragpurender.sys`，234,928 B，SHA256 `C8D28D4588ACBEF2C4B2ECEA652F653EC9B5FB5E5D3855846A10D562562C3D27`
  （INF `DriverVer 11/20/2026, 0.1.0.96`，cat 已重出并按 sys→cat 顺序签名，证书 `CN=SwitchWOA Test Driver Signing` A7C0A920…）
- 新增记录（全部只读探针）：
  `PreVout/PreVoutDvc/PreCtl1/PreCtl2/PreId1/PreId2/PreReg6/PreReg7`（0x1C 寄存器 0x00–0x07，写前）、
  `RailCtl1/RailCtl2/RailVoutDvcRead`（写后回读）、
  `Gpio6Before/Gpio6PueBefore/Gpio6PdeBefore`（MAX77620 0x3C/0x3E/0x3F 写前）、
  `Td1TglAck/Td1StatusOn/Td1Clamp/Td1TglAck2/Td1StatusOff/Td1RgAfter`（分区 13 两点 toggle）
- 孔径探针改为**先读 AUX 0x538F0000**（step 45/46）再读 BAR0（step 40–44），
  用于区分"GPU 完全不响应"与"仅 BAR0 解码路径死"。
- 本轮开关（已离线写入卡上 SYSTEM hive）：
  `Limit=1  Mask=0x80  RgWrite=0  RailSweep=0  ClampNeedsTd=1  RailProbe=1  ToggleProbe=1  ApertureProbe=0`
  → **刻意不碰孔径**，目标是一份完整可信的 trace + 三条新证据类别。
- 取证读取：关机 + UMS → `F:\SwitchWOA\gpu-run.txt`（现在最多 192 条）；寄存器视图用
  `F:\SwitchWOA\install-…` 之外只读的 `offline-inspect-gpu3.ps1`（需管理员）。

### 11.6 下一轮（按本轮结果分支）

1. `Td1StatusOn` 的 bit13 若变为 1 → 图形分区是 13 而不是 1，KMD 改走 13。
2. 若 13 也不上电，且 `Gpio6Before` 本来就是 0x09（EN 一直是高）→
   说明轨道侧完全不是软件开关问题，改走交接路线 **B（UEFI/PmicDxe 侧编程 + 屏显结果）**
   或 **C（装一份 L4T 取 GPU 可用时的 PMC/PMIC/GPCPLL 现场）**。
3. 孔径探针的 AUX-first 结果用于判定是否需要放弃 BAR0 直读路径。

**Gemini 独立审核**：本机仍无 `antigravity`/`gemini` 可执行 → 0.1.0.96 的差异记为**待审**。

## 12. 2026-11-20 20:2x：根因确定——轨门释放必须在电轨真的上电之后；GPU 孔径已经活了

### 12.1 决定性的一次实测（KMD 0.1.0.96，开关 `RgWrite=1 ClampNeedsTd=0 ApertureProbe=1`）

在此之前所有 `PMC+0x2D4`（GPU 轨门钳位）的写 0 尝试，都是在 **GPIO6 还是输入态**（`Gpio6Before=0x02`，VDD_GPU 关着）时做的，
所以必然挂总线。本轮先把电轨真正打开（`RailGpio6=0x09`、`RailVoutRead=0x9F`）**再**写 0x2D4，结果：

```
Step 0x17            <- 写 0x2D4 之前的最后一个标记
Step 0x18            <- 写返回了
RgWrote        0x00000000
GpuRgCleared   0x00000000     <- GPU_RG_CNTRL 已归零
ClampGateStatus 0x3FFECF4D
Step 0x2D  Aux0        0xEAFFFFFE   (AUX 窗口 0x538F0000，不是有效寄存器路径)
Step 0x28  Aperture0   0x12B000A1   <- NV_PMC_BOOT_0，GM20B 真值
Step 0x29  Aperture1   0xBAD00200
Step 0x2A  AperturePllCfg 0xBADF3000
Step 0x2B  AperturePllOut 0x81100000
```

**GPU 孔径从"一碰就挂"变成可访问**，`BootGpuMeasuredKhz` 之前一直为 0 的原因也随之明确。

### 12.2 结论

1. **根因是顺序**：`0x2D4 = 0`（释放 GPU 轨门）必须发生在
   `MAX77620 GPIO6 拉高（EN）→ MAX77621 VOUT 使能` **之后**；否则该写会停总线（WHEA / DPC_WATCHDOG）。
   Mariko 参考实现 `t214_power.c` 里这一步从来不出问题，正是因为 Mariko 交接时轨道本来就是通的。
2. **`PWRGATE_STATUS` bit1 是假信号**：即使 GPU 孔径完全可用，该位仍为 0
   （`TdStatusFinal = 0x3FFECF4D`）。L4T 的 `tegra_powergate_sequence_power_up()` 也从不检查它。
   分区 1 与分区 13(3D1) 的 toggle 都无效，说明"分区开关"这条线在本 SoC 上无法作为判据或入口。
3. **AUX 窗口（0x538F0000）不是可用的寄存器通路**（读到 0xEAFFFFFE），后续只用 BAR0（0x57000000）。
4. `Gpio6Before=0x02` + `PreVout=PreVoutDvc=0x3F`（1.0 V、使能位为 0）证明从前一轮 UEFI/PmicDxe 的
   GPIO6 拉高**没有留下效果**：到了 KMD 启动时该脚又回到输入态。KMD 自己拉高才是有效的。

### 12.3 已并入源码（KMD 0.1.0.97）

- `swt210_rg_write()` 默认值改为 **1**（写 0 释放轨门）；
- `swt210_clamp_needs_td()` 默认值改为 **0**（不再用 PWRGATE_STATUS 位去门控这一步）；
- 新增记录 `PwrGateAfterRg / ClampAfterRg / RgAfterWrite`；
- `tg_gpu_power_up()` 末尾加 `record_step(60)` 终止标记 —— 否则文件会停在最后一个"由开关控制"的步骤上，
  看起来像卡住（本轮 `Step 0x15` 就是这个假象）。
- 包：`kmd-package\tegragpurender.sys` 235,440 B，SHA256 `2D6A446B…BBC…A5A`（0.1.0.97，已签名）

### 12.4 新增工具（PC 侧）

- `erista-gpu\tools\push-kmd-live.ps1`：scp 包 → `pnputil /add-driver` 暂存 → 类键指向新 oemNN.inf →
  `ConfigFlags |= 0x20`（下次开机重装）→ 写开关 → 可选重启。**不做热装、不重启设备**（这两种历史上会卡死整机）。
- `D:\paopao\.tmp\pull-robust.ps1`：等"新的 LastBootUpTime"再拉 `gpu-run.txt` + Parameters（首个版本会因为
  "关机瞬间 SSH 还能应答"而误判，已修）。
- 控制台 SSH 账号 `woa` 是**已提权**（Administrators / High），可以直接改注册表和暂存驱动。

### 12.5 下一步（按 `HardwareStartLimit` 逐级放开）

| Limit | 到哪一步 | 判读 |
|---|---|---|
| 1 | 只上电（已通过） | `HardwareStage=105`, `PowerResult=0` |
| 2 | +时钟（`tg_gpu_clock_start`：GPCPLL + 校准 + 测频） | `ClockResult` 0/1/2/3/4/5，`BootGpuMeasuredKhz` 应 ≈ 460800 |
| 3 | +执行（MMU/通道/PMU/ACR/GR 固件） | `ExecutionResult`、`FrontendResult` |
| 4/5 | +图形模板 / 全量 | 之后才是 UMD/DWM |

已知风险点（下一轮若 `ClockResult=1 BAD_STATE`）：`t210_clock.c` 入口要求 GPCPLL 的 `CFG.ENABLE=0` 且 `VCO.bit0=0`，
若 Hekate/U-Boot 已经开过 PLL，需要改成接受现成频率而不是直接失败。

## 13. 2026-11-20 21:xx【关键突破】priv ring 未初始化 → GPCPLL/GPC 页被拒答

### 13.1 证据链

固定事实（KMD 0.1.0.96 ~ 0.1.0.102，`ApertureProbe=1`）：

| 区域 | 读回 | 结论 |
|---|---|---|
| `0x57000000` NV_PMC_BOOT_0 | `0x12B000A1` | 孔径本身可访问 |
| `0x57000200` NV_PMC_ENABLE | `0xC0012020` | PMC 可读；按 `hw_mc_gm20b.h`：PRIV_RING(0x20)=1、PWR(0x2000)=1、PFB(0x100000)=1、**PFIFO(0x100)=0、PGRAPH(0x1000)=0** |
| `0x57120058` / `0x57022454` / `0x57020160` / `0x57020798` | 真值 | PRIV/LTC/SLCG/THERM 正常 |
| `0x57137000/04/0c/18`、`0x57132800+` | `0xBADF3000` / `0xBADF5000` | **GPCPLL 核心页 + GPC 广播页整体拒答** |
| 写 0 或写两个不同值后读回 | 仍是 `0xBADF3000` | 不是缓存（映射为 `MmNonCached`，已验证），是 priv-ring 错误应答 |
| 打开 GPU 内部位 `0x100`(PFIFO) | PLL 页无变化 | 我先前把位号搞错了：`0x100` 是 PFIFO，不是 MC |

### 13.2 根因（switchroot nvgpu 源码，本地 clone `CTCaer/switch-l4t-kernel-nvgpu@linux-dev-r32.5`）

`gk20a.c` 的初始化顺序：`bus.init_hw → clk.disable_slowboot → **priv_ring.enable** → clk.init_clk_support（编程 GPCPLL）`。

```c
void gm20b_priv_ring_enable(struct gk20a *g)          // common/priv_ring/priv_ring_gm20b.c
{
        nvgpu_cg_slcg_priring_load_enable(g);          // 0x001200A8 = 0x0
        gk20a_writel(g, pri_ringmaster_command_r(), 0x4);        // 0x0012004C
        gk20a_writel(g, pri_ringstation_sys_decode_config_r(), 0x2); // 0x00122204
}
```

而 KMD 只在 `tg_gpu_frontend_start()` 里写这两条，**`PowerSkipMask=0x80` 一直把该函数跳过** → priv ring 从未初始化 →
SYS/GPC 寄存器空间（GPCPLL、GPC broadcast）全部返回 `0xBADF` 错误码；PMC/PRIV/LTC 不受影响，所以"部分可读"。
这也解释了为何 `t214_clock.c` 在 Mariko 上能用同样的 RMW 代码（Mariko 由 BPMP/固件把 ring 与 PLL 都配好了）。

### 13.3 修复（KMD 0.1.0.103）

- 新增 `priv_ring_init()`：`0x570120A8=0` → `0x5712004C=4` → `0x57122204=2`，
  在上电阶段（释放轨门之后）执行，**独立于前端开关**；开关 `PriRing`（默认 1）。
- 同一次启动里做 A/B 记录：`PriCfgBefore/PriCoeffBefore` vs `PriCfgAfter/PriCoeffAfter`、
  `PriRingCmd`、`PriDecodeCfg`、`PriDvfs0After`、`PriBcastAfter`。
- 包：0.1.0.103，`sys` 238,080 B，SHA256 `99E38C1C…9E118`（经 SSH 推 `oem89.inf`）。

### 13.4 另外两条本轮新事实

- **分区 13（3D1）的 toggle 会直接挂总线**（trace 停在 `Step 0x19` 之前，随后 WHEA/自动重启）。
  13 是"活"的图形域，与 1/23 不同 —— 以后不再触碰。
- **`Limit` 的关卡语义**（`windows/hardware.c`，`ClockFirst=0` 时）：
  `power → frontend(受 Mask bit7) → limit<3 返回 → clock_start → limit<4 返回 → boot_graphics → …`
  所以 `Limit=2` 根本到不了时钟阶段，必须 `Limit=3`。
- **轨门释放需要电轨已稳定**：新增 `RailSettleMs`（默认 25 ms），并在释放前记录 `RailVoutAtGate`（实测 `0x9F`）。

### 13.5 下一步

1. 确认 `PriCfgAfter` 不再是 `0xBADF3000` —— 若是，GPCPLL 门已开。
2. `Limit=3` 走完整时钟阶段：此时读回可信，RMW 安全；判读 `ClockResult` 与 `BootGpuMeasuredKhz`。
3. 时钟通过后 `Limit=4` 进执行阶段（MMU/通道/PMU/ACR/GR 固件）。

## 14. 2026-11-20 22:27–22:32：priv ring 确认是总开关；GPCPLL 打通，实测 460.8 MHz

### 14.1 priv-ring A/B（KMD 0.1.0.104，`Limit=1`，探测点放在 MC 排空之后 + 20 ms 稳定等待）

| 记录 | 前 | 后 |
|---|---|---|
| `PriCfgBefore/After` (0x137000) | `0xBADF3000` | **`0x0`** |
| `PriCoeffBefore/After` (0x137004) | `0xBADF3000` | **`0x14201`**（M=1,N=0x42,P=1 的真实系数） |
| `PriDvfs0After` / `PriBcastAfter` | `0xBADF3000` | `0x0` / `0x0` |
| `RwFirst/RwSecond`（写两个不同值再读） | 都是 `0xBADF3000` | **`0x12345` / `0x54321`** |
| `PriRingCmd` / `PriDecodeCfg` | — | `0x0`（命令自清）/ `0x2` |

结论：**priv ring 未初始化就是 GPCPLL/GPC 页拒答的唯一原因**；`PowerSkipMask=0x80` 跳过前端启动导致它从未被启用。
写入顺序（nvgpu `gm20b_priv_ring_enable`）：`0x570120A8=0` → `0x5712004C=4` → `0x57122204=2`。

### 14.2 时序教训（两次 WHEA 的成因）

- 在**释放轨门之后立刻**读 `0x57137000` 会挂总线（0.1.0.103 三次 WHEA 的成因）：SYS/GPC fabric 尚未稳定。
- 正确位置：MC 排空之后 + 20 ms 等待；priv-ring 写完后 3 ms 再回读。
- 轨门释放前需要 `RailSettleMs`（默认 25 ms）+ 记录 `RailVoutAtGate`（实测 `0x9F`）。

### 14.3 时钟阶段通过（KMD 0.1.0.104，`Limit=3`）

| 记录 | 值 | 含义 |
|---|---|---|
| `ClockResult` | **`0x0`** | TG_GPU_CLOCK_OK |
| `BootGpuMeasuredKhz` | **`0x70800` = 460800** | 与 `BootGpuTargetKhz` 完全一致，PLL 真的跑起来了 |
| `BootGpuRailUv` | `0xD418E` = 868,750 µV | CVB 按 speedo 推出的电压 |
| `HardwareStage` | `0xCD` (205) | 200 + Clock.stage |
| `ClkNote0/1` | `0x0` / `0x14201` | 保护读通过（PLL 页可读） |
| `ExecutionResult` | `0xFFFFFFFF` | 未执行（`Limit=3` 在此之前返回） |

`tg_gpu_clock_start()` 全流程走通：NA 模式、SDM=-4096、`CFG.ENABLE→SYNC→SEL_VCO` 切换、计数器测频。
机器稳定，无 WHEA。

### 14.4 当前状态与下一步

- 已通：ACPI/资源 → 电轨（GPIO6+MAX77621）→ 轨门释放（0x2D4）→ priv ring → GPCPLL（460.8 MHz 实测）。
- 开关：`Limit=4`（执行阶段：`tg_gpu_boot_graphics` = MMU/PFIFO/PMU/ACR/GR 固件），`Mask=0x80`，`RgWrite=1`，`ClampNeedsTd=0`。
- 待验证：`ExecutionResult`（`TG_GPU_EXEC_*`）、`FrontendResult`、ARENA/GR 固件加载。

## 15. 2026-11-20 23:39【里程碑】Erista GPU 内核侧 bring-up 全线打通

### 15.1 最终一次启动的实测值（KMD 0.1.0.109，`Limit=5`、`PowerSkipMask=0x00`）

| 记录 | 值 | 含义 |
|---|---|---|
| `HardwareStatus` | **`0x0`** | STATUS_SUCCESS：DdiStartDevice 成功 |
| `StatusWorker` / `StatusCleanup` / `BcDetail` | `0x0` / `0x0` / `0x0` | worker 起来、清理正常、StartHardware 成功 |
| `HardwareStage` | `0x13A` (314) | 300 + 执行子步 |
| `ClockResult` | `0x0` | 时钟阶段 OK |
| `BootGpuMeasuredKhz` | **`0x70800` = 460800 kHz** | GPCPLL 实测频率 = 目标频率 |
| `BootGpuRailUv` | `0xD418E` = 868,750 µV | CVB 电压 |
| `ExecutionResult` | `0x0` | MMU/PFIFO/PMU/ACR/GR 固件全通过 |
| `GrStage` / `GrMagic` | `9` / `0x600dc0de` | golden context 已保存 |
| `WorkerHwReady` | `1` | 硬件就绪、worker 已接管 |
| 设备管理器 | `Tegra X1 (GM20B) render adapter (experimental)`，**status=OK / problem=CM_PROB_NONE** | Code 0 |

### 15.2 最后一个坑：平台层会锁存第一个 I/O 失败

`windows/platform_io.c` 的 `failure()`：

```c
static NTSTATUS failure(TG_GPU_PLATFORM *p, NTSTATUS status)
{
    if(p && !NT_SUCCESS(status) && NT_SUCCESS(p->IoStatus)) p->IoStatus = status;
    return status;
}
```

而 `platform_adapter.c` 在硬件启动成功之后还有：

```c
if (NT_SUCCESS(status) && !NT_SUCCESS(a->Platform.IoStatus)) status = a->Platform.IoStatus;
```

→ 只要**任何一次**平台 I/O（PMIC 桥读写、孔径读写）失败过，即使 GPU 全链路成功，整个 StartDevice 也会被判失败。
本次的触发源是**诊断探针**：读 MAX77621 `0x02–0x07`、MAX77620 `0x3E/0x3F` 时 switchfan 桥返回 `INVALID_PARAMETER`。
教训：**诊断读取只能用桥支持的寄存器**（MAX77621 `0x00/0x01/0x06`、MAX77620 `0x3C/0x40`），或把诊断读取放到不锁存 IoStatus 的路径上。

### 15.3 完整链路（现在每一段都有实测支撑）

```
ACPI/资源 → 上电(MAX77620 GPIO6 + MAX77621 800→869 mV)
  → 释放轨门(PMC+0x2D4=0，须电轨稳定后 + RailSettleMs 25 ms)
  → priv ring 初始化(0x1200A8=0 / 0x12004C=4 / 0x122204=2)      ← 关键门
  → GPCPLL(NA 模式 + SDM=-4096 + CVB 校准 + SEL_VCO) → 实测 460.8 MHz
  → MMU/PFIFO/PMU/ACR(安全启动)/GR 固件
  → USERD 映射 → golden GR context(0x600dc0de) → 图形模板
  → worker 启动 → DdiStartDevice = STATUS_SUCCESS（设备 Code 0）
```

计时要点：PLL 页的读/写必须等到 MC 排空之后（+20 ms），更早会挂总线。

### 15.4 现在的位置与下一阶段

- **A 阶段（内核侧 bring-up）完成。** 这不等同于"GPU 能显示"：`tegragpuno3d.dll` 目前仍是 2 KB 占位，DWM 依旧走软件路径。
- **B 阶段**：
  1. KMD 提交路径与同步契约：GPFIFO 提交流、完成 fence、`DxgkCbSynchronizeExecution`/时间线信号量、residency；
  2. UMD（tgd3d / Vulkan ICD）实现 DDI；
  3. DWM 合成与共享资源（redirected flip / MPO / 共享面所有权）；
  4. 之后才是 DXVK/D3D11 游戏。
- 开关现状（建议保持不变直到 B 阶段有结论）：
  `HardwareStartLimit=5`、`PowerSkipMask=0x00`、`RgWrite=1`、`ClampNeedsTd=0`、`ApertureProbe=0`、`RailProbe=0`、`ToggleProbe=0`、`RailSettleMs=25`、`PriRing=1`。

## 16. 2026-11-21 下午：P0-1 GPU 逐档提频上机（460.8 → 691.2 → 768 → 844.8 全绿）

HANDOFF 第 10 节 P0-1 的验收轮。目标是把"460.8 MHz 的算力天花板"摸清：每档只改一个数、重启一次、
读回实测频率/电压/阶段码。结论：**零售 OPP 阶梯的最高档 844.8 MHz 在真机上全绿通过，921.6 MHz 被 CVB
电压上限挡住（需要 DTS/电压域改造，不属于本轮）**。

### 16.1 本轮代码改动（KMD 0.1.0.110，三处，共 40 行）

| 文件 | 改动 | 为什么 |
|---|---|---|
| `windows/hardware.c` | 新增 `hw_boot_khz()`：读 `Parameters\BootGpuKhz`（76800..921600 且为 76800 的整数倍才接受，否则回落到编译期 `TG_GPU_RENDER_KHZ`）；`Status.target_khz` 与两处 `tg_gpu_clock_start()` 调用点改用它；新增记录 `BootGpuKhzReq` | 频率档位变成**运行时可调**：每档只改注册表＋重启，不再"改源码 → 重编 → 重签 → 装机"。这是把一轮从 ~25 分钟压到 ~6 分钟的关键 |
| `platform/t210/t210_clock.c` | 白名单从 `{FIXED, LOW_POWER, HIGH}` 改成 `t210_gpu_index(target) != T210_GPU_OPP_COUNT`（即 OPP 表里任意一档合法，表外仍拒）；新增 `ClkNote3/4/6`（目标频率 / CVB 电压 / speedo）、`ClkNote5`（external_calibration）、`ClkNote7`（实测频率）和 `mark 84` | 原来只放行三个固定值，691.2/768 会被判 `BAD_STATE`；另外每档要在 `gpu-run.txt` 里留下"跑在什么点、CVB 给了多少 mV、标定值是多少" |
| （只读）`windows/render_miniport.c` | 未改。`BootGpuTargetKhz` 仍是编译期常量，**实际档位看 `ClkNote3`/`BootGpuMeasuredKhz`** | 保持"少改一处"；下次固化默认值时一并改成 `Status.target_khz` |

### 16.2 四档实测（KMD 0.1.0.110，开关：`Limit=5`、`Mask=0x00`、`RgWrite=1`、`ClampNeedsTd=0`、`PriRing=1`、`RailSettleMs=25`）

| `BootGpuKhz` | 实测 `BootGpuMeasuredKhz` | `BootGpuRailUv` | `ext_cal` | `ClockResult` | `ExecutionResult` | `GrStage`/`GrMagic` | `StatusWorker` |
|---|---|---|---|---|---|---|---|
| 460800（基线） | **460800** | 868,750 µV | 23 | 0 | 0 | 9 / `0x600dc0de` | 0 |
| 691200 | **691200** | 1,000,000 µV | 36 | 0 | 0 | 9 / `0x600dc0de` | 0 |
| 768000 | **768000** | 1,050,000 µV | 40 | 0 | 0 | 9 / `0x600dc0de` | 0 |
| 844800 | **844800** | 1,106,250 µV | 46 | 0 | 0 | 9 / `0x600dc0de` | 0 |

四档全部：`HardwareStatus=0x0`、`HardwareStage=0x13A`、`PowerResult=0x0`、`ExecStage=0xE`、
`WorkerHwReady=1`，设备节点仍然 Code 0。**实测频率 = 目标频率，误差 0**（计数器每 800 个 38.4 MHz 参考
周期计数一次，`measured = count × 48`，所以三档都落在整数 kHz 上）。

### 16.3 CVB 模型被真机证实（speedo = 2031）

`gpu_speedo` 读到 **2031**（`ClkNote6`，`0x7EF`）。把 `t210_opp.c` 的 CVB 公式按 speedo=2031 手算，
与四档实测电压逐位对上：

| 频率 | CVB 原始值 | 实际下发（MAX77621 6.25 mV 对齐） | 预测 vs 实测 |
|---|---|---|---|
| 460800 | 864,688 µV | **868,750 µV** | 完全一致 |
| 691200 | 995,567 µV | **1,000,000 µV** | 完全一致 |
| 768000 | 1,046,504 µV | **1,050,000 µV** | 完全一致 |
| 844800 | 1,100,504 µV | **1,106,250 µV** | 完全一致 |
| 921600 | 1,158,770 µV | — | **超过 `max_uv` 1,113,000 → `t210_gpu_cvb_uv()` 返回 0** |

`external_calibration` 也随之单调上升（23 → 36 → 40 → 46），全部在 ±64 的合法窗内，
说明 `DVFS2[6:0]` 的外部标定没有成为高档位的瓶颈。

**为什么 921.6 不能测**：CVB 要求的 1,162,500 µV 超过零售 `max_mv = 1113`。而
`tg_gpu_clock_start()` 在 `required_uv == 0` 时会走"沿用 PMIC 当前电压"的回退分支——
也就是说现在这个实现会**在 ~869 mV 上强行开 921.6 MHz 的 PLL**，这是危险的。
下一轮应当把这个回退收紧（有 speedo 时 `required_uv==0` 直接返回 `RAIL_ERROR`），
921.6 MHz 本身要等 DTS/电压域改造（HANDOFF 提到的 `gpu-dvfs-rework`）。

### 16.4 本轮新增的工具（都在 `D:\paopao\WoA\erista-gpu\tools\` 与 `D:\paopao\.tmp\`）

| 用途 | 脚本 |
|---|---|
| 打包+签名（一步到位，顺序已固化） | `WoA\erista-gpu\tools\package-kmd.ps1 -Version 0.1.0.xxx`：换 sys → 改 INF `DriverVer` → **先签 sys** → `Inf2Cat` → 再签 cat，最后打印 sys/cat 的 SHA256 |
| 在线推送 + 设档位 | `tools\push-kmd-live.ps1 -Version .. -Limit 5 -Mask 0x0 -RgWrite 1 -ClampNeedsTd 0 -BootGpuKhz 691200`（新增 `-BootGpuKhz`，0 = 不动） |
| 只改档位 + 重启 | `.tmp\set-boot-khz.ps1 -Khz 691200`（顺带把工作档开关重新压一遍，防止半还原状态） |
| 等重启后取数据 | `.tmp\pull-run.ps1 -ExpectKhz 844800`：用 `ClkNote3 == 期望档位` 且 `ClkNote7 == 期望档位` 判定"这次开机跑到了这一档"，并对 `gpu-run.txt` 做"大小稳定后再读"（该文件是整文件覆盖写，直接读会读到半截） |
| 抗丢包执行远端命令 | `.tmp\rsh-retry.ps1`、`.tmp\rsh.ps1`（远端命令写在 `remote.txt` 里，避免多层 shell 引号） |

### 16.5 环境更正：SSH 连不上**不是** sshd 状态问题

本轮两次出现"TCP 能连、banner 超时"甚至"认证成功但远端 shell 不返回任何输出"。先后有两次
在控制台 `net stop sshd & net start sshd` 之后恢复，一度以为是 sshd 卡住。加入探针任务后可以确认：

- 控制台**本地**探测（TCP 127.0.0.1/LAN 地址读 banner + spawn 子 PowerShell 计时）**一直是通的**；
- 失败只发生在**从本机（PC）发起的连接**上，且同一分钟内重试 6 次里第 6 次成功。

→ 结论：这是 **Wi-Fi 链路丢包/超时**（该机器 ping 从来不通、但 SSH 时好时坏），不是服务状态。
处理办法是**客户端重试**（`rsh-retry.ps1`，`ConnectTimeout=30`，20 秒一轮），而不是频繁重启 sshd。
另外仍在卡上留了一个自愈任务 `SwitchWOA-SshdKeeper`（开机 + 每 2 分钟跑 `C:\SwitchWOA\sshd-keeper.ps1`，
**只有本地探针失败时才**重启 sshd，健康时不打断会话），日志 `C:\SwitchWOA\sshd-keeper.log`。

### 16.6 本轮之后的位置

- **P0-1 完成**：零售阶梯 460.8 / 691.2 / 768 / 844.8 四档真机全绿，电压由 CVB 自动给出，
  档位切换只走注册表，可随时回退（`set-boot-khz.ps1 -Khz 460800`）。
- 卡上当前档位：**844800**（零售默认档）。
- 下一项：**P0-2 上屏链路微基准**（纯 blit / 半转 / 缩放各 N 次计时 → 换算上屏链路理论最大 fps），
  用来回答"20 fps 是算力不足还是上屏链路代价"。这一项不需要 UMD。

### 16.7 v0.1.0.111：CVB 上限守卫 + PC 侧主机测试（2026-11-21 19:0x）

16.3 留了一个口子：`required_uv == 0` 时 `tg_gpu_clock_start()` 会"沿用 PMIC 当前电压"继续开 PLL。
有 speedo 时这不是"未知"而是"超出 `max_mv`"，等于在 ~869 mV 上强开高档位。本轮修掉：

```c
if (c->gpu_speedo && (!required_uv || required_uv > 1113000u))
    return TG_GPU_CLOCK_RAIL_ERROR;   /* 不再回退到在飞电压 */
```

验证方式不再靠重启（一次开机 6~8 分钟＋SSH 还丢包），改成 **PC 侧主机测试**：把
`platform/t210/t210_clock.c` 和 `t210_opp.c` 原样编进一个 x64 控制台程序，用寄存器/PMIC/计数器模型驱动。

| 脚本 | 说明 |
|---|---|
| `WoA\erista-gpu\tests\test_clock_t210_host.c` | 真实 `.c` 直接 include；断言四档电压/频率/`ext_cal`、921600 被拒且**未使能 PLL**、表外频率 `BAD_STATE`、低档（76.8/307.2 MHz）仍在 `VMIN` 800000 µV |
| `WoA\erista-gpu\tools\run-host-tests.ps1` | clang-cl + `/nodefaultlib` 只链 Kernel32（本机无 MSVC vcruntime，故测试自带 `host_main`/`assert`/`WriteFile` 输出） |

主机测试输出（与真机实测逐位一致）：

```
  460800 kHz: rail 868750 uV, measured 460800 kHz, ext_cal 27
  691200 kHz: rail 1000000 uV, measured 691200 kHz, ext_cal 40
  768000 kHz: rail 1050000 uV, measured 768000 kHz, ext_cal 45
  844800 kHz: rail 1106250 uV, measured 844800 kHz, ext_cal 51
921600 kHz refused (CVB 1,162,500 uV > max 1,113,000 uV)
off-ladder frequencies refused
T210 clock ladder + CVB envelope guard tests passed.
```

真机侧 v0.1.0.111 已装上并复测（`BootGpuKhz=844800`，sys SHA256 `26846120…`）：
`HardwareStatus=0x0`、`Bc=6`、`BcDetail=0`、`ClockResult=0`、`ExecutionResult=0`、`GrStage=9`、
`ClkNote3=0xCE400`、`ClkNote4=0x10E14A`（1,106,250 µV）、`ClkNote7=0xCE400`、`Step=84`。

> 抓取时的一个现象值得记住：**新包装机会把 INF 里的三个默认值写回**（`InitializeStatus`/`ResourceStatus`/
> `HardwareStatus` = `0xFFFFFFFF`），所以"刚装完包、驱动还在跑启动序列"的那一瞬间读注册表，会看到
> `HardwareStatus=0xFFFFFFFF` + `Step` 停在中途 + 旧的 `ClkNote*`——这是**正常的中间态**，不是失败。
> 判据永远是 `Step=84` + `ClkNote7 == ClkNote3 == 目标`（`BcDetail=0`）。
## 17. 2026-11-21 19:0x：上屏链路的 CPU 侧实测（不动驱动）

P0-2 本来要用 KMD 的诊断入口跑全屏操作计时，但那需要一个**新的可重复提交循环**（改驱动）。
在动手改驱动之前先把**不需要驱动就能量到的部分**量完：这一轮的结论已经足够回答"20 fps 是算力不足
还是上屏链路代价"——**是上屏链路的 CPU 侧代价**，而且量级对得上。

### 17.1 当前显示栈（先取证，避免拿错前提）

| 项 | 实测 |
|---|---|
| 桌面模式 | **1024×768 @ 96 DPI**（不是面板原生 1280×720，也不是 720×1280） |
| 显示驱动 | `Microsoft Basic Display Driver`（`ROOT\BASICDISPLAY\0000`）+ `Microsoft Basic Render Driver` |
| Display 类适配器 | **没有**（`{4d36e968-…}` 下只有 `Configuration` 子键）；`HKLM\HARDWARE\DEVICEMAP\VIDEO` 的 `\Device\Video0` 指向 `Services\BasicDisplay` |
| 我们的 GPU | 只是 `render adapter (experimental)`（WDDM render-only），**不参与上屏** |
| CPU | 4×Cortex-A57，`Win32_Processor` 报 MaxClockSpeed 1786 MHz（固件表值，实际运行点未验证） |

→ 含义：今天所有画面合成、旋转、缩放、以及往 framebuffer 的拷贝全在 **CPU + BasicDisplay** 上，
GPU 完全不参与。所以"20 fps"先要在这条链路上找答案。

### 17.2 测量方法（可复现，不含驱动）

`WoA\erista-gpu\tests\screenbench.c` → `tools\build-screenbench.ps1` 编成 **ARM64 原生 exe**
（`--target=aarch64-pc-windows-msvc`、`/nodefaultlib`、只链 Kernel32、自带入口与 `memcpy`，
不需要 CRT、不需要签名），scp 到卡上 `C:\SwitchWOA\screenbench.exe` 直接跑。
计时用 `QueryPerformanceCounter`（19.2 MHz）；缓冲区 1280×720 BGRA = 3.6 MB；
同一份 exe 连跑 3 次取范围，其它进程干扰的那一次会被明显看出来（首轮 `rot90 ×4` 出现过 8.4 ms 的离群值，
三连测后稳定在 5.3–6.1 ms）。

### 17.3 单帧 1280×720 的 CPU 代价（3 次运行范围，µs/帧）

| 操作 | 1 线程 | 4 线程 | 说明 |
|---|---|---|---|
| 平面拷贝（本程序逐字循环） | **2404–2537** | 2497–2529 | 4 线程**没有任何收益** |
| 平面拷贝（`RtlMoveMemory`） | **2281–2293** | — | OS 的 memcpy 和上面的循环一样快，说明已到内存带宽上限 |
| 512 KB（留缓存）平面拷贝 | **79–80** | — | 6.5 GB/s → 说明瓶颈是 DRAM，不是核心 |
| 同向逐行拷贝（带 pitch） | 2394–2594 | — | 与平面拷贝基本相同 |
| **90° 旋转（朴素）** | **22071–23273** | — | 目的地址跨行写，最坏情况 |
| **90° 旋转（32×32 分块）** | **12831–13711** | **5304–6109** | 分块本身就快 1.7×；再上 4 线程共快约 4× |
| 缩放 1280×720 → 1024×768（最近邻） | 2302–2669 | — | 只读一次源、写一次目的 |
| **缩放+旋转 一趟做完** | **23341–24681** | — | 比"先旋转再缩放"更差：跨行写主导 |
| SrcOver 混合两帧（DWM 那种） | 6567–7267 | 4673–4959 | 4 线程快约 1.4× |

关键两点：

1. **帧拷贝已经贴着 DRAM 走**：3.6 MB 单线程 2.3–2.5 ms ≈ 1.5 GB/s（读+写 3 GB/s），
   4 线程不涨、512 KB 留缓存时快 30 倍 → 有效带宽有限，**任何"每帧多拷一次"的设计都要按 2.4 ms 记账**。
2. **旋转是最贵的一步**，而且实现方式差 1.7–4 倍：朴素 22–23 ms、分块 12.8–13.7 ms、分块×4 线程 5.3–6.1 ms。

### 17.4 结论：20 fps 的账算得平

把"旋转 + 缩放 + 拷到 framebuffer"按**单线程**加起来：
`13.7 + 24.9 + 2.6 ≈ 41.2 ms/帧 → 24 fps 上限`（三次运行 40.4–41.2 ms，24–25 fps）。

这和实测到的 ~20 fps 是同一个量级：**瓶颈在上屏链路的软件代价，不是 GPU 算力**。
而且这条链路的代价几乎全部来自"每帧在 CPU 上搬 3.6 MB ×2~3 次"和"旋转的跨行写"。

可操作的改进顺序（下一轮直接照这个做）：

1. **旋转换成 32×32 分块 + 4 线程**：13.7 ms → 5.3 ms，白拿 8 ms；
2. **别做"缩放+旋转"的一趟版本**（23 ms），先旋转再缩放（13.7+2.4=16 ms）反而更快；
3. **减少每帧的整帧拷贝次数**（现在 rotate+scale+copy 各一遍 = 3 次读+3 次写 3.6 MB）；
4. 长期才是 P2-6：把旋转/缩放下沉到 DC 硬件（`t210_dc.h` / `panel_vpvr`），CPU 一点都不碰。

### 17.5 这一轮**没有**测到的（避免误判）

- **真实 present 延迟**：需要 ETW（`Microsoft-Windows-Dwm-Core`）或真实 D3D 应用，本机没有 wpr/UMD。
- **BasicDisplay 往 framebuffer 的拷贝代价**：`SetDIBitsToDevice` / `BitBlt` 直写屏幕 DC 在 DWM 下
  落到重定向表面，测出来 0.24 ms/帧（4,444 fps）——这个数字**不可信**，不要引用。
  （用 GDI+ `DrawImageUnscaled` 全屏则测得 28.4 ms/帧，含 GDI+ 自己的格式转换，同样不能当作 present 代价。）
- **GPU 提交/完成延迟**：需要 17.3 里 P0-2 的提交循环（改驱动）。

> 另外把 460.8 MHz 档位重新设为当前工作点（`BootGpuKhz=460800`，`Measured=460800`、`Clock=0`、
> `Exec=0`、`Hw=0`、`Step=84` 已验证），本轮的 CPU 测量就是在这个档位下做的。

## 18. 2026-11-21 19:4x：CPU 主频实测 + 旋转"实现方式差 4 倍"（仍然不动驱动）

第 17 章把"上屏链路 CPU 代价"量出来了，但有两个问题必须补上才能下结论：
① CPU 到底跑在哪个档（1024 MHz 还是 1786 MHz）；② 那段 13 ms 的旋转是**硬件下限**还是**我的写法差**。

### 18.1 CPU 主频实测：约 1750 MHz（零售 boost 档，不是 1024）

方法：`QueryThreadCycleTime()` 返回线程自己的周期计数器，`cycles / 墙钟秒` 就是运行中的核心频率。
先做了一次**反证**——`Sleep(50ms)` 期间周期计数只走了 42,295（≈0.85 MHz），说明它是**真·核心周期计数器**，
不是拿墙钟换算的（否则睡眠期间也会涨）。随后三段 250 ms 忙循环：

| 段 | cycles | 墙钟 | 推算主频 |
|---|---|---|---|
| 1 | 436,954,036 | 250,280 µs | **1,745,854 kHz** |
| 2 | 436,397,941 | 250,047 µs | **1,745,257 kHz** |
| 3 | 437,550,134 | 250,023 µs | **1,750,038 kHz** |

→ **CPU 已经在 1786 MHz（boost/零售高性能点）附近跑**，没有"再去提 CPU 档"这样的便宜可捡。
（`Win32_Processor.MaxClockSpeed=1786`、`CurrentClockSpeed=1786` 这次和实测一致。）

意义：第 17 章那些 ms 数是**已经踩着 boost 档**量出来的，所以"20 fps"不能用"CPU 跑在低频"解释。

### 18.2 90° 旋转：同一算法换写法，1 线程 24 ms → 5.2 ms

之前的"32×32 分块"版本在**内层循环里做 `d[x*dw + (h-1-y)]` 的 64 位乘加**，每像素一次地址计算。
换成**指针递推**（目的地址连续递增、源地址按 stride 递增，一行 32 像素 = 128 B 刚好是整条 cache line）
之后：

| 90° 旋转 720×1280 → 1280×720 | 1 线程 | 4 线程 |
|---|---|---|
| 朴素（源序读、目的跨行写） | 20.2–24.4 ms | — |
| 分块 32×32（内层每像素乘加） | 11.3–14.0 ms | 4.8–6.3 ms |
| **指针递推（`op_rot90_fast`）** | **5.15 / 5.16 ms** | **2.66 / 2.75 ms** |

1 线程就赢 2.2–4.7 倍，4 线程共 8 倍。**这不是硬件进步，是写法进步。**

### 18.3 "缩放+旋转一趟做完"是死路（两种写法都慢）

| 缩放+旋转 1280×720 → 768×1024 | 时间 |
|---|---|
| 一趟完成（内层每像素除乘） | 23.3 / 23.3 ms |
| 一趟完成＋指针递推（`op_scale_rot_fast`） | 18.3 / 19.1 ms |
| **分开做：旋转（fast 5.15）＋缩放（2.36）** | **≈7.5 ms** |

原因：缩小+旋转时每次读源像素都落在**新的 cache line**（64 B 里只用 4–8 B），读写放大到 ~16×，
再怎么递推指针也是 DRAM 带宽限制。→ **结论：不要融合，先旋转再缩放；要融合就只能靠 GPU。**

### 18.4 修正后的单帧预算（1280×720，1 线程，CPU 1750 MHz）

| 路径 | 组成 | 每帧 | 上限 |
|---|---|---|---|
| 朴素实现（分块旋转 + 融合缩放旋转 + 拷贝） | 12 + 23 + 2.4 | **≈37–41 ms** | 24–27 fps |
| **指针递推实现（旋转 + 缩放 + 拷贝）** | 5.15 + 2.36 + 2.37 | **≈9.9 ms** | **≈100 fps** |
| 指针递推 + 旋转上 4 线程 | 2.7 + 2.36 + 2.37 | ≈7.4 ms | ≈135 fps |

→ **"20 fps" 里至少 25 ms/帧 是纯实现浪费**，不是硬件上限，也不是 GPU 算力不足。
这条结论比第 17 章更硬：同一个算法、同一台机器、同一档 CPU，只是把地址计算从内层挪出去。

### 18.5 交付物（可直接拿去改上屏链路）

`WoA\erista-gpu\tests\screenbench.c` 里两个**可移植 C99 参考实现**（无平台依赖，只有整型和指针）：

- `op_rot90_fast()` / `op_rot90_fast_range()`：目的连续写、源按 stride 递推，1 线程 5.15 ms，4 线程 2.66 ms；
- `op_scale_rot_fast()`：融合版的"已优化形态"，仍是 18 ms → 用来证明**融合不划算**。

给上屏链路/UMD 的建议顺序：① 旋转换成 `op_rot90_fast` 的写法；② 旋转与缩放分开，别融合；
③ 旋转按目的行切 4 段多线程；④ 减少每帧整帧拷贝次数（每次 2.4 ms 都是实数）。

### 18.6 新增工具与仍缺的东西

| 工具 | 用途 |
|---|---|
| `tests\cpuprobe.c` → `tests\arm64-bin\cpuprobe.exe`（卡上 `C:\SwitchWOA\cpuprobe.exe`） | 用 `QueryThreadCycleTime` + `Sleep()` 反证法测实际核心频率 |
| `tests\screenbench.c`（更新） | 增加指针递推旋转、融合缩放旋转、4 线程对照 |

仍然缺（都要改驱动或要 ETW/UMD，不在"不需要驱动"的范围里）：真实 present 延迟、BasicDisplay 往
framebuffer 的拷贝代价、GPU 提交/完成延迟。

## 19. 2026-11-21 19:5x：显示栈最后一环取证（合成关闭 / 32bpp / 60 Hz，且"到屏"代价在用户态测不到）

第 17–18 章把 CPU 侧代价量清楚了，但有两个数字一直站不住：`SetDIBitsToDevice` 测出 0.24 ms/帧、"present
28 ms/帧"。这一轮把它们查清，并给出**这条链路上真正能当作判据的三个事实**。

### 19.1 三个实测事实

| 项 | 实测 | 依据 |
|---|---|---|
| **DWM 合成是关闭的** | `DwmIsCompositionEnabled() = False` | 所以 `DwmFlush()` 立即返回（0.22 ms/次），也解释了为什么"直写屏幕 DC"能生效——没有重定向表面 |
| **帧缓冲格式** | 1024×768、**32bpp**、planes=1、**vrefresh = 60 Hz** | `GetDeviceCaps(HORZRES/VERTRES/BITSPIXEL/PLANES/VREFRESH)` 于屏幕 DC |
| **真实 present 代价在用户态测不到** | `SetDIBitsToDevice` 32bpp 全屏：0.46 / 0.14 ms/帧，`lines_copied=768/768`（调用是成功的） | 3.1 MB / 0.46 ms = 6.8 GB/s、0.14 ms = 22 GB/s——超过 DRAM 带宽，说明**写入只是落在缓存/写合并区**，扫描输出由 DC 异步完成，没有可等待的"到屏"事件 |

同理，之前那次 GDI+ `DrawImageUnscaled` 全屏 **26.6–27.0 ms/帧**（`Draw+DwmFlush` 27.02、`draw-only` 26.61，
两者几乎一样 → 与 flush 无关）**不是显示代价，而是 GDI+ 自己做格式转换/合成的 CPU 代价**。

### 19.2 于是"20 fps"的完整账（都以实测为准）

- 面板/模式上限：**60 Hz → 每帧预算 16.7 ms**。低于 60 就是 CPU 或程序自己的问题。
- 朴素实现的上屏链路 CPU 代价：**37–41 ms/帧 → 24–27 fps**（第 18 章）；
- 指针递推实现：**≈9.9 ms/帧 → 装得进 16.7 ms 预算，能跑满 60 Hz**（旋转 5.15 + 缩放 2.36 + 拷贝 2.37）；
- 再叠加 GDI+ 那种"每帧转换一遍"的代价（26.6 ms）就直接掉到 37 fps 以下——**这就是 20 fps 的来源**：
  不是 GPU 算力，也不是"到屏"慢，而是**每帧在 CPU 上做的搬运/转换**。

### 19.3 由此可以给下一步的硬结论

1. 想要 60 fps：**先别碰驱动**，把上屏链路换成 `op_rot90_fast` 那套写法（5.15 ms）+ 分开做缩放（2.36 ms）
   + 只在最后拷一次（2.37 ms），总量 9.9 ms，留 6.8 ms 给程序自己。
2. 不要用 GDI+/`DrawImageUnscaled` 这类"每帧转换一遍"的接口做全屏 present（26.6 ms，占满两帧预算）。
3. 显示栈本身的刷新是 60 Hz，`DwmFlush` 不可用作帧率节拍（合成为关，它立即返回）——**要测真实帧率只能靠
   程序自己计数，或者等驱动/UMD 提供 fence**。
4. 若将来要把这 9.9 ms 也省掉，就得改驱动（DC 硬件旋转/overlay）或让 GPU 做 blit——那是 B/C 阶段的事。

> 本轮同时确认：**机器上没有任何 Tegra 显示/面板驱动**（`C:\Windows\System32\drivers` 里只有
> `tegra210gpio.sys`、`tegra210i2c.sys`、`tegragpurender.sys`；`oem*.inf` 里没有 Display 类、也没有
> NVIDIA/Tegra 提供者）。所以"打开一个已有的硬件旋转通路"这条路**不存在**，硬件旋转必须新写显示小端口。

## 20. 2026-11-21 20:2x：面板竖屏/Windows 横屏的旋转到底谁在付代价（含取证）+ 交付可移植旋转实现

### 20.1 先把"谁在转"查清（真机注册表 + 构建参数）

| 事实 | 值 | 来源 |
|---|---|---|
| 面板/主表面存储 | **720×1280（竖屏）** | `HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\Configuration\*\00`：`PrimSurfSize=720x1280`、`ActiveSize=720x1280` |
| Windows 施加的路径旋转 | **`Rotation=2`** = `D3DKMDT_VPPR_ROTATE90`（枚举从 0=UNINITIALIZED 起，1=IDENTITY） | 同上 |
| 缩放模式 | `Scaling=4` = `ASPECTRATIOCENTEREDMAX`（等比缩放 + 居中留边） | 同上 |
| 当前逻辑桌面 | **1024×768**（`GetSystemMetrics`/`GetDeviceCaps`/`Screen.Bounds` 三处一致） | 实测 |
| UEFI 侧旋转 | v72 构建带 **`-D SWITCH_FB_ROTATE=2`** | `WoA\build\build-gpu0.sh` 第 23 行；`SimpleFbDxe.c` 注释：模式 0 = 1280×720 横屏给 UEFI 控制台用，**模式 1 = 720×1280 原生线性帧缓冲给 OS loader**，所以 Windows 拿到的就是竖屏原生缓冲 |
| 合成 | `DwmIsCompositionEnabled()=False` | 第 19 章 |

→ 所以现在的真实情况是：**Windows 逻辑上是 1024×768 横屏，主表面是 720×1280 竖屏，路径要做 ROTATE90 + 等比缩放**，
而合成是关的 → 这一步由显示路径（BasicDisplay）在每帧做，既不在 DWM 里，也不在 GPU 里。
而且 1024×768 ≠ 面板的 1280×720，**缩放是多余的一步**。

### 20.2 结论：不需要"先渲染竖屏再旋转"，旋转只发生一次，代价取决于谁做

- 程序（D3D/DWM）永远按**当前模式的逻辑方向（横屏）**渲染；旋转发生在**下游一次**。
  "先渲染竖屏再转成横屏"是**多做一遍**：既付整帧渲染，又付整帧旋转，纯亏。
- 那"一次旋转"可以落在四个地方，代价差 1~2 个数量级：

| 谁来做 | 代价（1280×720 一帧） | 说明 |
|---|---|---|
| **DC 硬件窗口变换/块线性** | ≈0 | Tegra DC 支持窗口 90/180/270 变换；这台机器的 UEFI 已经用 `SWITCH_FB_ROTATE` 把方向问题解决了 |
| **GPU（CE 拷贝引擎 / 3D blit）** | 亚毫秒~1 ms，且不占 CPU | 我们 KMD 的 `windows\present.c` + `core\present_blit.c` 就是这条路：90/270 编码成 CE 列条纹拷贝，180 用一个 halfturn 包 |
| **显示驱动的软件路径（现状）** | 见下（CPU 量级） | 合成关闭时由 BasicDisplay 做 ROTATE90+缩放 |
| **CPU（程序自己转）** | 5.1 ms（优化后）/ 12–24 ms（朴素） | 第 18 章实测 |

- 单帧 CPU 代价（1750 MHz，实测，三次一致）：旋转 `rot90 fast` **5.12 ms**、4 线程 **2.69 ms**；
  缩放 1280×720→1024×768 **2.28 ms**；整帧拷贝 **2.39 ms**（`RtlMoveMemory` 2.26 ms）；
  "缩放+旋转一趟" 18–23 ms（两种写法都慢，别融合）。

### 20.3 立刻可做的一条优化（不用改驱动）

把 Windows 显示模式设成 **1280×720**：
- 现在 1024×768 + `ROTATE90` → 旋转后是 768×1024，再被 `ASPECTRATIOCENTEREDMAX` 缩放到 720×1280（还带留边）；
- 改成 1280×720 后，`ROTATE90` 得到的就是 **720×1280，与主表面 1:1**，**缩放这一步完全消失**、像素也不再被重采样。
- 这是纯用户设置，改完可以用同一套工具对比（`screenbench.exe` 的 scale 项与实际帧率）。

### 20.4 交付：可移植的旋转/缩放参考实现（已像素级自测）

`WoA\erista-gpu\tests\present_ref.c` + `present_ref.h`（纯 C99、无平台依赖，内核/用户态都能用）：

```c
tg_present_rot90(dst, 720, src, 1280, 1280, 720);      /* 横屏 1280x720 -> 竖屏 720x1280 */
tg_present_rot90_range(dst, dst_pitch, src, src_pitch, sw, sh, x_from, x_to);  /* 线程切片用 */
tg_present_scale_nearest(dst, dst_pitch, dw, dh, src, src_pitch, sw, sh);
```

要点（照这四条写就是这个速度）：① 内层不做任何乘法，指针递推；② 以**目的行区间**切分给多线程；
③ 目的端每次连续写 32 像素 = 128 B ≥ 1 条 cache line；④ **旋转与缩放分开做**。

`tools\run-host-tests.ps1` 里已接入自测（PC 上跑，不占开机轮次）：

```
rotate 1280x720 -> 720x1280 storage: pixel-exact vs reference
rotate 93x57 with padding: pixel-exact vs reference
rotate split in 4 row ranges: identical to one-shot
scale 1280x720 -> 1024x768: pixel-exact vs reference
PASS present_ref
```

> 自测第一天就抓到一个真 bug：分块内目的列号是**递减**的（`sh-1-y`），我原来的写法在块内升序填，
> 每块内部像素被倒序。`screenbench.c` 里的同名实现也已按同样方向修正，重跑确认耗时不变（5.12 ms）。

### 20.5 顺带修掉一个自己挖的坑

之前那个 sshd 自愈任务在 CPU 满载时会误判"本地 banner 探测失败"并**重启 sshd**，把正在跑的会话打断
（20:13–20:18 连续三���，害得 benchmark 跑到一半被杀）。现在改成**只启动、不重启**：
服务没跑就 `Start-Service`，正在跑就只记日志，绝不动它。日志 `C:\SwitchWOA\sshd-keeper.log`。

### 20.6 A1 结果：当前档位 40 分钟稳定性 soak —— 全绿

`C:\SwitchWOA\soak.ps1`（SYSTEM 计划任务，每 30 s 采样一次）跑满 40 分钟（19:51:12 → 20:31:29），
**71 个样本全部为绿**，日志 `C:\SwitchWOA\soak.log`（本机备份 `D:\paopao\.tmp\soak-result.txt`）：

```
19:51:14 up=2261s dev=OK/problem=0 hw=0x0 clock=0 khz=460800 step=84 gr=9 worker=0 freeMB=2,188 faults=0
   ...（71 个样本全部相同形态）...
20:30:59 up=4646s dev=OK/problem=0 hw=0x0 clock=0 khz=460800 step=84 gr=9 worker=0 freeMB=2,257 faults=0
=== soak end 2026-11-21 20:31:29 ===
```

判据与结论：

| 项 | 结果 |
|---|---|
| 设备状态 / ProblemCode | 全程 `OK` / `0`（没有掉设备、没有 Code 43） |
| `HardwareStatus`/`ClockResult`/`ExecutionResult`/`GrStage`/`StatusWorker` | 全程 `0x0 / 0 / 0 / 9 / 0`（不变） |
| `BootGpuMeasuredKhz` | 全程 `460800`（PLL 没有掉频/漂移） |
| uptime | 单调递增 2261 s → 4646 s（**中途没有重启/蓝屏**） |
| WHEA / BugCheck / Kernel-Power 事件 | 全程 `faults=0` |
| 可用内存 | 2.15–2.28 GB 之间波动，无下降趋势（无泄漏迹象） |

→ **在当前工作档（`Limit=5`、`Mask=0`、460.8 MHz）连续跑 40 分钟零故障**。
这也把 P1-4 那条"WHEA 还是 DPC_WATCHDOG"的对照基准立住了：稳定状态下两者都不出现。
`soak.ps1` 留在卡上，随时可以复制计划任务加长到几小时；`BootGpuKhz=844800` 下的同款 soak 建议放到 A4 再做。
## 21. 2026-11-21 20:4x：B2 启动自检 + B1 开关默认值（KMD 0.1.0.112，真机验收）

### 21.1 改了什么

| 文件 | 改动 |
|---|---|
| `windows/render_miniport.c` | 启动末尾写一条汇总记录 **`SelfCheck`**：`1`=全绿（时钟/执行/golden context/实测频率=目标）、`2`=故意闸住（`HardwareStartLimit<5` 或电轨未取得，不算失败）、`0`=失败。省掉每次人眼看十几条记录 |
| `windows/platform_adapter.c` | `hw_start_limit()` 的缺省值 **0 → 5**（完整 bring-up）。理由：P0 阶梯 + 40 分钟 soak 已证明这条路径稳定，而"忘了设注册表就什么都不做"才是更大的隐患；失败阶段仍是返回错误而不是卡启动，`install-safe.cmd` 照旧写 `HardwareStartLimit=1` 做保守档 |
| `windows/hardware.c` | 只改注释（指向 `platform_adapter.c` 的缺省值） |

`t210_power.c` 里其余默认本来就已经是工作档：`RgWrite=1`、`PriRing=1`、`RailSettleMs=25`、`PowerSkipMask=0`、
`ClampNeedsTd=0`、`RequireTdStatus=0`、各探针 `0` —— 本轮确认无需改动。

### 21.2 真机验收（最严格的那种：把开关全删掉）

1. 装 `0.1.0.112`（`oem97.inf`，卡上 sys SHA256 `0C78DA6A…`）；
2. **把注册表里所有手工开关删干净**（`HardwareStartLimit`/`PowerSkipMask`/`RgWrite`/`ClampNeedsTd`/
   `ApertureProbe`/`RailProbe`/`ToggleProbe`/`ClockFirst`/`RequireTdStatus`/`RailSettleMs`/`PriRing`/`BootGpuKhz`/`RailSweep`）；
3. 重启，读回：

```
SelfCheck=1  HardwareStatus=0x0  ClockResult=0  ExecutionResult=0  GrStage=9  measured=460800  target=460800
设备: OK / problem=0
注册表里剩下的人工开关: []      ← 只有驱动自己写的 RailSettleMs/PriRing 记录
```

→ **驱动只靠自己的缺省值就能完成全链路 bring-up（460.8 MHz、Code 0）**，`SelfCheck=1`。
以后再装新卡/换包，不需要先"记得设一堆开关"；要看是否全绿也只需要读一个值。

> 注意：`RailSettleMs` 和 `PriRing` 会出现在 Parameters 里，那是 `t210_power.c` 的 `record32()` 记录（观测值），
> **不是**输入开关——判断"有没有人工开关"时要把这两个排除。

### 21.3 卡上现状（本轮结束）

| 项 | 值 |
|---|---|
| KMD | **0.1.0.112**（`oem97.inf`，sys `0C78DA6A…`） |
| 开关 | **全空**（走缺省：Limit=5 / Mask=0 / RgWrite=1 / ClampNeedsTd=0 / PriRing=1 / RailSettleMs=25 / BootGpuKhz=460800） |
| 状态 | `SelfCheck=1`、`HardwareStatus=0`、`ClockResult=0`、`ExecutionResult=0`、`GrStage=9`、`measured=460800`、设备 `OK/problem=0` |
| 卡上工具 | `screenbench.exe`、`cpuprobe.exe`、`soak.ps1`、`sshd-keeper.ps1` + 两个自愈任务（ssh 只启动不重启 / soak 按需启用） |
## 22. 2026-11-21 21:0x：能不能让 DC 原生就"横屏"？（决定性问题，纯文档/代码侧核对）

用户提问：既然 DC 旋转最便宜，能不能让显示链路面板侧原生就是横屏，好让 Windows 桌面和所有程序
**彻底不再付这次旋转**？—— 答案：**可以，这就是 T210 的 "column scan"，但这个项目 2026-11-14 把它撤了**。

### 22.1 依据（都在 `docs\PORTING.md`，不用猜）

| 事实 | 原文位置 |
|---|---|
| 面板光栅固定 720×1280（DSI 输出尺寸不变），横屏是靠 **Window A 列扫描** 实现的 | `PORTING.md` 311-317：T210 的 L4T 表给 Display A Window A 声明了 **column scan + packed-format rotation**，其窗口编程"**swaps the prescaled axes and DDA inputs for column scan, retaining the output raster dimensions**" |
| 这不是改个 Windows 方向标志就行 | 同上："this is more than changing a Windows orientation flag" |
| 列扫描要求 **block-linear** 扫描面，不能沿用现在的 pitch-linear（pitch 2880） | `PORTING.md` 317-319："**Hekate also has a Switch-specific block-linear column-scan configuration**. Its stride, block height and offsets belong to that framebuffer layout and **must not be transplanted onto our current pitch-linear allocation**" |
| 正确实现要一起改：源尺寸、row pitch、allocation 描述、VidPN/MPO rotation、真正的扫描变换；并保留 backing 直到 active bank 确认，释放时恢复原布局 | `PORTING.md` 301-324 |
| **2026-11-14 用户改选了 GPU prerotation**：DC column scan 与 decrement transform **被移除**，改成把横屏预旋转进普通 720×1280 竖屏主表面；"physical output has not been tested" | `PORTING.md` 293-299 |
| 仍然有效的最终目标 | `PORTING.md` 301："The final orientation requirement was **hardware rotation and a 1280x720 landscape desktop**"；`docs\STATUS.md` 942 同义 |
| 我们的 KMD 侧现在只接受竖屏 identity | `windows\panel_scanout.c` 的 `storage_matches()`：只收 `D3DDDI_ROTATION_IDENTITY` + 720×1280 + pitch 2880；`platform\t210\t210_dc.h`：transform 位"只用于清除继承的 transform，**从不用来旋转扫描**" |

### 22.2 三条路线的真实代价（同一帧 1280×720）

| 路线 | 每帧代价 | 谁要付 | 落地难度 |
|---|---|---|---|
| **DC column scan**（用户问的"原生横屏"） | **≈0**（fetch 引擎按列读，无拷贝、无 CPU/GPU） | 硬件 | 最高：显示小端口 + block-linear 分配 + latch/恢复；**且物理输出从未验证** |
| **GPU（CE/3D）在 present 里转** | 亚毫秒（3.6 MB 一读一写，异步，不占 CPU） | GPU 拷贝引擎 | 中：不需要 block-linear；`core\present_blit.c` 已把 90/270 编成 CE 列条纹拷贝 |
| **今天的现状（BasicDisplay 软件路径 + 1024×768 缩放）** | **5.1–24 ms**（实测） | CPU | 已经是现状，也就是那 20 fps 的来源 |

### 22.3 另外发现：这台机器现在跑的并不是这个项目的目标配置

`docs` 里的逻辑桌面目标是 **1280×720 横屏 identity**；而本机实测是 **1024×768 + `ROTATE90` + `ASPECTRATIOCENTEREDMAX`**
（STATUS 20.1）——即现在走的是 BasicDisplay 的兜底模式，额外多了一次缩放。
→ 零风险、零驱动改动的一步：把 Windows 模式设成 **1280×720**，`ROTATE90` 后正好等于主表面 720×1280，
缩放那一步直接消失。这是通往上面任何一条路线之前都该先做的一步。

### 22.4 建议顺序

1. **现在**：Windows 模式改 1280×720（去掉缩放，0 成本）；
2. **下一步（中）**：GPU 在 present 路径做旋转（拿掉 BasicDisplay 的 5–24 ms CPU 代价，回到 GPU 亚毫秒）；
3. **再往后（大）**：DC column scan + block-linear 扫描面（彻底 0 代价，但必须先验证物理输出，且要与 VidPN/MPO rotation 契约一起改）。
## 23. 2026-11-21 21:2x【重要更正】SSH 会话是 session 0，屏幕相关测量必须进 session 1

用户从设置界面截图看到的是 **桌面模式 1280×720 / 活动信号模式 720×1280**，与我第 19–20 章写的
"1024×768、合成关闭" **不一致**。查清了：**是我测错了上下文**。

### 23.1 错误原因

Windows 上通过 SSH 跑的命令在 **session 0（服务会话）**，那里有一块**合成出来的 1024×768 虚拟显示**，
而且**没有 DWM**。所以第 19 章的那些读数全是 session 0 的：

| 我之前写的 | 实际含义 |
|---|---|
| "帧缓冲 1024×768 / 合成关闭 / 60 Hz" | **session 0 的虚拟显示**，不是面板 |
| "`DwmFlush` 立即返回、`SetDIBitsToDevice` 0.14–0.46 ms 到屏" | 写的是 session 0 的虚拟帧缓冲（内存），与真实上屏无关 |
| "GDI+ 全屏 26.6 ms/帧" | 那是 session 0 里的 GDI+ 转换开销，**不能代表真实上屏** |

用**以登录用户身份运行的计划任务**（`-Principal switch\woa -LogonType Interactive`）在 session 1 里重测：

```
[probe] session=1  user=switch\woa
screen: 1280x720 32bpp 64Hz   metrics=1280x720   composition=True      ← 与用户截图一致
DwmFlush x60: 22.03 ms/frame => 45.4 fps
DwmFlush（5 秒长采样）: 231 帧 / 5.03 s => 21.78 ms/frame (45.9 fps)
dwm cpu = 0.00 s / 5 s => 0%     System cpu = 0.00 s / 5 s => 0%      system total cpu = 2%
```

### 23.2 由此得到两个更硬的结论

1. **合成本身几乎不耗 CPU（0%）**，桌面空转时 `DwmFlush` 稳定 21.78 ms/帧 → 这个节奏是
   **面板当前的扫描率（≈45.9 Hz）**，不是软件旋转在吃 CPU。也就是说：**现在这台机器有一个 ≈46 fps 的硬天花板**，
   与旋转无关，与 Windows 设置无关，来自**显示时序**。
   - 旁证：`t210_dc.h` 里的面板 raster 是 720+72+72+136 = 1000 像素/行、1280+1+9+10 = 1300 行/帧 = 1,300,000 像素；
     若 PCLK 约 59.4 MHz，则 59.4M / 1.3M ≈ **45.7 Hz**，与实测 45.9 fps 吻合。
   - 要跑 60 Hz，同样的 raster 需要 **78 MHz** PCLK（或缩短 porch）。这是 **UEFI/显示初始化侧（hekate/DisplayInitLib）**
     的常数，不是 Windows 驱动的事。
2. `GetDeviceCaps(VREFRESH)` 报的 "64 Hz" 只是 BasicDisplay 的估算值，**不能当刷新率依据**。

### 23.3 修掉测量方法（可复用）

屏幕/合成相关的任何测量，**必须在交互会话里跑**：

```powershell
$act = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -File C:\SwitchWOA\session-probe.ps1'
$prn = New-ScheduledTaskPrincipal -UserId 'switch\woa' -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName 'SwitchWOA-SessionProbe' -Action $act -Trigger (New-ScheduledTaskTrigger -Once -At (Get-Date).AddSeconds(5)) -Principal $prn -Force
```

卡上已留 `C:\SwitchWOA\session-probe.ps1` 与 `session-probe2.ps1`（输出 `session-probe.txt` / `session-probe2.txt`）。
**纯 CPU/内存基准（`screenbench.exe`、`cpuprobe.exe`）不受影响**，那些数字仍然有效。

### 23.4 对"20 fps"结论的影响（诚实修订）

- 第 17–18 章的 **CPU 能力测量仍然成立**：同一台机器上旋转 5.12 ms（优化）/ 12–24 ms（朴素）、缩放 2.28 ms、
  整帧拷贝 2.39 ms。这些是"如果 CPU 来做要付多少"。
- 但**"当前 20 fps 就是这 5–24 ms 的旋转造成的"这个说法现在没有证据支持**：桌面空转时 DWM 占用 0%。
- 现在的候选原因是三个，需要下一步分别量：① ≈46 Hz 的**扫描率天花板**；② 程序自己（游戏）的每帧代价；
  ③ 只有当画面真的在变时才会出现的**变换路径代价**（需要按"真正的 present"测，不能用直写屏幕 DC 的方法）。

### 23.5 下一步（按收益/成本）

1. **把面板扫描率修到 60 Hz**（PCLK/porch，UEFI 侧）→ 天花板 46 → 60 fps，对**所有**程序 +30%；
2. 在 session 1 里用**正常呈现路径**（D3D/WARP 或带脏矩形的 GDI 窗口）量真实每帧代价；
3. 之后再谈旋转下沉（GPU present blit → DC column scan）。
## 24. 2026-11-21 21:5x：显示时序查清了（不是 46 Hz），真瓶颈是"全屏 present 只有 13 fps"

### 24.1 PCLK/刷新率这条线索：查错方向，但结论是好消息

先查"扫描率是否只有 46 Hz"：

| 检查 | 结果 |
|---|---|
| UEFI 是否自己初始化显示 | **否**。`SimpleFbDxe` 只从 PCD 取帧缓冲（`PcdMipiFrameBufferWidth=720`、`Height=1280`、`Address=0xF5A00000`），注释写明 "Retrieve simple frame buffer from **pre-SEC bootloader**"；`DisplayInitLib.h` 存在但**没有任何 .inf 链接它** → DC/DSI/PLLD/时序由 **hekate** 编程，不在本仓库 |
| 真实 vsync 间隔 | **15.71 ms（≈63.7 fps）**：640×360 GDI 窗口稳定跑满 vsync（probe3/probe4 两次一致：16.31 / 15.71 ms） |
| 之前 `DwmFlush` 的 21.3 ms 是什么 | **不是刷新率**，是"合成器响应节奏"：加满 CPU 负载后跌到 **50.15 ms（19.9 fps）** → 证明它受 CPU 影响，不能当刷新率用 |

→ **面板本身是 ~60 Hz 级，不需要去改 hekate 的 PLLD/porch**（省掉一件高风险的固件改动）。

### 24.2 真正的瓶颈：全屏 present 只有 13.2 fps

用**正常窗口 present 路径**（不是直写屏幕 DC）在 session 1 里量，同一份位图、只改窗口大小：

| 窗口 | 每帧 | fps |
|---|---|---|
| 640×360（0.23 Mpx） | **16.31 ms** | **61.3**（= vsync 上限，说明小更新几乎免费） |
| **1280×720（0.92 Mpx，桌面大小）** | **75.97 ms** | **13.2** ← 这里 |

- 像素数只涨 4×，时间涨 4.7×，且绝对值远超"每像素拷贝"应有的量级：
  **76 ms / 921,600 像素 ≈ 82 ns/像素**，而同一台机器 CPU 做一次完整旋转只要 5.12 ms（5.6 ns/像素）。
- 也就是说：**小窗口更新是 vsync 限（61 fps），一旦全屏更新就掉到 13 fps**，
  多出来的 ~60 ms/帧不是绘制，而是**全屏变换/合成/上屏那一段**。
- 这正好解释了"20 fps"这种观感：游戏/模拟器每帧都是全屏更新。

### 24.3 下一步的决定性实验（只差一个对照）

要证明那 ~60 ms 就是"竖屏面板的旋转/变换"，只要做一次 A/B：
**把桌面临时切到纵向 720×1280（identity rotation）**，再跑同一个全屏 present 测试。
- 若掉到 ~20 ms 量级 → 旋转确认是主因，方案就是"旋转下沉到 GPU 或 DC"；
- 若仍是 70 ms 级 → 瓶颈在 BasicDisplay 的全屏拷贝/格式转换，方向改为换显示驱动。

这个实验会让屏幕短时间变成竖屏（可随时切回），所以留到用户确认后再做，脚本已备好
（`C:\SwitchWOA\session-probe4.ps1`，只需换分辨率参数再跑一次）。

### 24.4 方法学沉淀（本轮最重要的一条）

- **屏幕/合成相关的测量必须在交互会话（session 1）里做**：用
  `-Principal <登录用户> -LogonType Interactive` 的计划任务 + 脚本落到 `C:\SwitchWOA\`。
- **`DwmFlush` 节奏 ≠ 刷新率**（受 CPU 负载影响：46.9 → 19.9 fps）；要测刷新率用
  "小窗口 vsync 限速"法（640×360 稳定 61–64 fps 即说明 vsync 正常）。
- **不要直写屏幕 DC**（`Graphics.FromHwnd(IntPtr.Zero)`）：合成开启时那是病态路径
  （session 1 实测 4.5 **秒**/帧），只能用来测"错误用法"，不能代表 present。
- 卡上脚本：`session-probe.ps1`（基础 caps/合成）、`session-probe2.ps1`（DWM/System CPU）、
  `session-probe3.ps1`（负载对照 + vsync 法 + 小窗口）、`session-probe4.ps1`（窗口尺寸对照）。
## 25. 2026-11-21 22:1x【第二次更正】全屏 present 不是 13 fps —— 那是 GDI+ 的锅，真实上屏 ~45 fps

第 24 章把"全屏 present 13.2 fps"当成瓶颈，**这个结论错了**：那 76 ms/帧几乎全是
**GDI+ `DrawImageUnscaled` 自己的开销**，不是显示路径。用真正的应用接口（GDI）重测：

| 尺寸 | `BitBlt` 仅入队 | `BitBlt` + 每帧 `DwmFlush`（等合成） |
|---|---|---|
| 640×360 | **0.18 ms**（5,644 fps） | **21.16 ms/frame = 47.3 fps** |
| **1280×720（全屏）** | **0.17 ms**（5,894 fps） | **22.07 ms/frame = 45.3 fps** |
| 对比：GDI+ 全屏（probe5c） | — | 63–76 ms/frame = 13–15 fps |

三件事一下就清楚：

1. **入队代价与面积无关**（0.17 ms，全屏也一样）——GDI 把 damage 交给 DWM，真正的合成在别处；
2. **真实上屏 1280×720 能到 45.3 fps**，和小窗口（47.3 fps）几乎一样 → **全屏更新不是瓶颈**；
3. 之前那 71.5 ns/像素（≈125 周期/像素）是 **GDI+ 的格式转换**，属于"选错 API"，不是机器慢。

### 25.1 由此得到的真实"上屏代价"

| 项 | 数值 |
|---|---|
| vsync 间隔（小窗口 vsync 限速法） | **15.71 ms（63.7 Hz）** |
| 全屏每帧 `BitBlt + DwmFlush` | **22.07 ms（45.3 fps）** |
| ⇒ 合成 + 变换 + 扫描这一段 | **≈ 6.4 ms/帧** |

也就是说：**上屏链路每帧只花约 6 ms，能撑 45 fps**；它**不是** 20 fps 的原因。

### 25.2 那 20 fps 最可能是什么

- **GDI+ 类接口**：这台机器上 GDI+ 全屏 63–76 ms/帧（13–15 fps）。任何"每帧用 GDI+ 画一帧"的程序
  （不少模拟器/自研界面就是这么写的）会被直接打到 15 fps 左右，和"20 fps"同一量级。
- 程序自己的渲染/逻辑开销（需要按程序自身的呈现路径量，不在本仓库范围）。

### 25.3 对前几章结论的最终修订

| 章节 | 原结论 | 现状 |
|---|---|---|
| 17（CPU 能力） | 旋转 5.12/12–24 ms、缩放 2.28、拷贝 2.39 | **仍成立**（同一台机器、同样 CPU 频率） |
| 19（"合成关闭/到屏 0.14 ms"） | —— | **作废**：那是 session 0 的虚拟显示（第 23 章） |
| 24（"全屏 present 13 fps 是上屏瓶颈"） | —— | **作废**：GDI+ 开销（本章） |
| 新的净结论 | —— | 上屏链路 ≈6 ms/帧、45 fps；GDI+ ≈71 ns/px（慢 15×）；旋转 A/B 因驱动只暴露单一模式+方向而无法做 |

### 25.4 下一步优先级（按新结论重排）

1. **确认朋友那边程序用的是不是 GDI+ / 每帧格式转换**——若是，换 GDI BitBlt 或 D3D，15 → 45 fps 是免费的；
2. 再谈把旋转下沉（DC column scan / GPU present）：现在它只值 ~6 ms/帧，收益比原先估计小一个数量级；
3. 面板时序（PCLK/porch）**不用动**——vsync 实测 63.7 Hz 正常（第 24 章）。

### 25.5 方法学追加（血泪版）

- **测量必须用"真实应用会用的接口"**：GDI+ ≠ GDI ≠ D3D，本机上差 15 倍；
- **换一个接口交叉验证**：同一操作至少用两种 API 各测一次（本次就是靠 BitBlt 对照才发现 GDI+ 的坑）；
- **入队 ≠ 上屏**：`BitBlt` 返回只代表 damage 已交；要等 `DwmFlush`（或统计 vsync）才是真实帧率；
- 驱动只暴露**单一模式+单一方向**（`ChangeDisplaySettingsEx` 对 720×1280 与 orient=0 都返回 `DISP_CHANGE_BADMODE`），
  所以"改设置做 A/B"这条路在当前驱动下走不通，只能靠显示小端口。