# 初代机（Erista / T210）DVFS 与 GPU 移植基础表

> 目标归属：长期目标 `goal-f5f0b435-65d1-4a21-aa8f-87844b217a52`（初代机 Windows 11 ARM64 原生 Vulkan GPU 硬件加速）。
> 本文是"先建表"的产物：把**初代机的 CPU / EMC / GPU 频率-电压-训练点表**先固化下来，作为后续按每台机器体质（Speedo/process）与超频能力适配的依据。
> 状态标记：**[本地已验证]** = 本仓库文档/源码可查；**[待抄录]** = 已知来源但原值未落盘；**[待实测]** = 必须在这台机器上跑出来。

数据来源（本地）：

- `D:\paopao\WoA\ref\dts\kernel-dts\nx-platforms\tegra210-odin-common.dtsi`（switchroot 初代机 odin DTS：cpufreq、emc-scaling-data、GPU max）
- `D:\paopao\WoA\ref\dts\kernel-dts\nx-platforms\tegra210-odin-emc.dtsi`（自动生成的 EMC 训练表）
- `D:\paopao\WoA\CPU频率选项-20261109.md`（本机 CPU/EMC 实测与 hekate 超频参数）
- `D:\paopao\WoA\winx\Drivers\UsbPowerAuditDxe\UsbPowerAudit.c`、`docs\hayato41200\uefi-revisions\R364-README-ja.md`（本机 Speedo/电压实测）
- `D:\paopao\WoA\mesa-switch-woa\tegra-x1-wddm\docs\*.md`（上游 GPU 工程文档，Lite 基线）

---

## 0. 先读每台机器的"体质"（这一步必须在表之前）

同一型号的初代机，硅体质不同，可稳定工作的最高频率/最低电压不同。所有表都要按**本机实测的 Speedo/process** 选行。

| 项 | 来源 | 说明 |
|---|---|---|
| CPU Speedo ID | L4T dmesg / hekate；本机实测 **ID 3**（R364-dmesg） | 决定 CPU OPP 电压行 |
| CPU Process | 同上：本机 **0** | process 0/1 两档，电压表不同 |
| GPU Speedo / Process | fuse 读取（见第 4 节） | 决定 GPU OPP 电压行 |
| SKU | 本机 **0x83**（R364-dmesg） | 注意：初代机与 Lite 都可能报 0x83，**不能只看 SKU** |
| HIDREV | `0x70000804`：初代机 `0x211x`，Lite/T210B01 `0x212x` | 社区代码用 `(HIDREV & 0xFFF0)==0x2120` 判 Lite；初代机分支正是 `0x211x` |
| EMC 体质 | 逐档爬坡实测（1600→1866→2000→2133） | 每台机器能稳多少不同 |

> 读取途径（Windows 侧尚未落盘）：目前最省事是在 Hekate / L4T Ubuntu 里读一次并记录；WinX UEFI 侧可参照 `UsbPowerAudit.c` 现有 MAX77621 读取逻辑扩展出 Speedo fuse 读取（fuse 块 `0x7000F800`，具体偏移以 `tegra210-soc.dtsi` 的 `fuse@7000F800` 与 L4T `fuse.c` 为准）。**[待实测]**

---

## 1. CPU OPP 表（T210，odin DTS）

来源：`tegra210-odin-common.dtsi:384-409` **[本地已验证]**。

`freq-table`（kHz）：

```
102000 204000 306000 408000 510000 612000 714000 816000 918000
1020000 1122000 1224000 1326000 1428000 1581000 1683000 1785000
OC: 1887000 1989000 2091000
```

- `max-frequency = 2091000`，`max-safe-frequency = 1785000`，`preserve-across-suspend;`
- 即：**官方最高 1785.6 MHz，可超到 2091 MHz**（OC 三档）。
- 已知电压（本机实测）**[本地已验证]**：
  - 开机固定 1017.6 MHz = **1025 mV**（MAX77621 VOUT code `0xC3`；R365 之前是 `0xB7`=950 mV）
  - 1785.6 MHz = **1227 mV**（ODN 表；R364-dmesg 引 L4T `TEGRA210_CPU_CVB_TABLE_ODN`：speedo 无关值，最小 825 mV、最大 1227 mV）
  - **L4T CPU FV 表已提取**（speedo 5/2/1/默认 四组，min/max 见下）：
    `51000→850 102000→850 204000→850 307200→850 403200→850 518400→850 614400→868 710400→912 825600→962 921600→1006 1036800→1062 1132800→1118 1228800→1168`（mV）
    来源：`src/l4t-kernel/extracted/tegra210-dvfs.c`（`CPU_FV_TABLE` / `cpu_lp_fv_dvfs_table`）
- 电源轨：**MAX77621，I2C5 `0x1B`，步进 6.25 mV**（`docs/Design-CPU-Clock-ja.md`）**[本地已验证]**
- 其余 OPP 电压：**[待抄录]** L4T `drivers/soc/tegra/tegra210-cpufreq.c` 的 `tegra210_cpu_cvb_table_odn`（17 档 × process 0/1）与 OC 扩展档。

> 表结构建议（与 Lite `dfll_tables.h` 的 OPP 行对齐，但数值用 T210 自己的）：

| 频率 (kHz) | OPP 行 | 电压 (mV) proc0 / proc1 | 状态 |
|---|---:|---|---|
| 1017600（boot） | — | 1025 / 待补 | 已验证 |
| 1785600（safe max） | — | 1227 / 待补 | 已验证 |
| 1887000 / 1989000 / 2091000 | OC | 待抄录 | 待抄录 |
| 其余 102–1683 | 17 档 | 待抄录 | 待抄录 |

---

## 2. EMC 训练点（LPDDR4）

来源：`tegra210-odin-emc.dtsi` 的 `emc-table@` 节点 **[本地已验证]**，DTS 内受支持训练点共 10 档：

```
40800  68000  102000  204000  408000  665600  800000  1065600  1331200  1600000
```

- 默认/出厂：**1600 MHz**（hekate/coreboot 默认档，UEFI 不碰 EMC）。
- **超频档（hekate ini，非 DTS）**：`ram_oc=1866000 / 2000000 / 2133000`，`ram_oc_vdd2=1200`（mV）——`CPU频率选项-20261109.md` **[本地已验证]**；2133 MHz 在 Ubuntu 上用过，属"已知可稳"起点，但**每台机器要自己爬坡验证**。
- 每档最小电压（从 `tegra210-odin-emc.dtsi` 逐档提取，**[本地已验证]**）：

| 频率 (kHz) | 40800 | 68000 | 102000 | 204000 | 408000 | 665600 | 800000 | 1065600 | 1331200 | 1600000 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `emc-min-mv` | 800 | 800 | 800 | 800 | 812 | 825 | 825 | 837 | 850 | 887 |
| `gk20a-min-mv` | 1100 | 1100 | 1100 | 1100 | 1100 | 1100 | 1100 | 1100 | 1100 | 1100 |

- OC 档（1866/2000/2133）不在 DTS 训练表内，hekate 侧用 `ram_oc_vdd2=1200`（mV）；逐档电压需重训练后实测。
- 注意：OC 档（1866/2000/2133）不在 odin DTS 训练表里，是 hekate 在**开机重训练**时加入的；Windows 侧要设 EMC 需要 UEFI/驱动配合（当前 UEFI 不碰 EMC）。

---

## 3. CPU→EMC 带宽下限表（T210，odin DTS，与 Lite 的关键差异）

来源：`tegra210-odin-common.dtsi:413-422` **[本地已验证]**：

| CPU (kHz) | 102000 | 204800 | 307200 | 403000 | 710400 | 921600 | 1581000 |
|---|---:|---:|---:|---:|---:|---:|---:|
| EMC 下限 (kHz) | 68000 | 102000 | 204000 | 408000 | 665600 | 1600000 | **2133000** |

对比 Lite/B01（`wifi-ref` 的 `emc_client_floor.h`，CPU 1581 → **3200**）：**初代机因 LPDDR4 训练上限在 2133，最后一档是 2133 而不是 3200**。这直接说明：

- 初代机做 CPU 变频时，EMC 下限最高只能托到 2133 MHz；
- 若把 Lite 的 CPU 变频逻辑直接搬来，`sw_emc_cpu_floor_khz` 必须换成上表（且上限钳到 2133）。

---

## 4. GPU（GM20B）频率/电压表

**硬件事实** **[本地已验证]**：

- 初代机 GPU = **GM20B**（与 Lite 同一颗 20nm Maxwell，256 CUDA），但 **T210 不是 T210B01**：HIDREV `0x211x`、时钟树/电源/中断路由不同。
- DTS 声明 `dvfs { nvidia,gpu-max-freq-khz = <921600>; }`（`tegra210-odin-common.dtsi:426-430`）——**初代机 GPU 的 retail regulator 硬上限 921.6 MHz**；Lite/B01 用 HighOPT 表可到 1228.8。
- **时钟源**（`ref/k_tegra210.dtsi:277-292`）：`clocks = GPU, PLL_P_OUT5 ("pwr"), PLL_G_REF ("ref")`；`resets = tegra_car 184`；`iommus = mc TEGRA_SWGROUP_GPU`。LIC 四组控制器在 `0x60004000/0x60004100/0x60004200/0x60004300`。
- 电源轨：**MAX77621（I2C5 `0x1C`，6.25 mV 步进）**；Lite 是 **MAX77812 M1**（由 SwitchFan 通过 SPB 桥接）——这是上游 GPU KMD **不能直接照搬**的第一原因。
- 固件：GR 用 `NETB_img.bin / fecs.bin / gpccs.bin`（`tegra21x`/`gm20b`），ACR 用 NVIDIA **32.7.6** 的 `nv_acr_ucode_prod.bin / pmu_bl.bin / gpmu_ucode_image.bin / gpmu_ucode_desc.bin / pmu_sig.bin / fecs_sig.bin`（`tegra-x1-wddm/docs/DRIVER.md:11-17`）。GM20B 相同，**固件大概率可复用**，但 T210 的 PMC/安全启动流程与 T214 不同，需要在 T210 平台层里验证（nouveau `nvkm/engine/device/tegra.c` 的 T210 支持路径）。

**OPP 表**（L4T `drivers/soc/tegra/tegra210-dvfs.c`）**[已提取到本地，可解析]**
- 来源：`D:\paopao\erista-gpu\src\l4t-kernel\extracted\tegra210-dvfs.c`（`git show` 提取）+ `gpu-dvfs-tables.txt`（表区段副本）
- 已确认的关键事实：
  - GPU retail 默认 `gpu_dvfs.max_freq = 844800`、`max_mv = 1113`、`pll_min_millivolts = 918`（speedo 4）；DTS `gpu-dvfs-rework` 可把上限改到 921.6 MHz
  - GPU CVB 表三张（`NA_FREQ_CVB_TABLE` / `NA_FREQ_CVB_TABLE_XA` / `FIXED_FREQ_CVB_TABLE`），行 = `{freq, {}, {c0..c5}}`，电压由 `cvb.c` 的 CVB 公式按 speedo/温度计算（公式见 `ERISTA-CLOCK-PORT.md` §2）
  - CPU FV 表（`CPU_FV_TABLE`）：`{51000,850} {102000,850} {204000,850} {307200,850} {403200,850} {518400,850} {614400,868} {710400,912} {825600,962} {921600,1006} {1036800,1062} {1132800,1118} {1228800,1168}`（mV）
  - 参考实现：`clk-tegra210.c` 已提取（PLLGP/C1 寄存器序列，供平台层 `clk.c` 使用）；`cvb.c`（CVB 数学）已提取

> 表结构建议（GPU，与 Lite `integration/switch-lite` 的 HighOPT 表同构）：

| GPU 频率 (kHz) | 电压 mV proc0 | proc1 | proc2 | 备注 |
|---|---:|---:|---:|---|
| 76800 … 921600（12 档） | 待抄录 | 待抄录 | 待抄录 | retail 上限 921.6（DTS） |
| （超频）>921600 | — | — | — | 初代机 retail regulator 硬上限；需评估能否像 CPU 一样通过电压表上调（hekate/驱动侧不保证） |

---

## 5. 上游 Lite GPU 驱动能否"先上机用着"——评估结论

**结论：不能直接装到初代机上用；但可以复用它的大部分软件，先把"步骤 1（真实 GPU 执行与回读）"在初代机上跑起来。**

理由（逐条，均有本地证据）：

| # | 事实 | 影响 | 证据 |
|---|---|---|---|
| 1 | **初代机 DSDT 没有 GPU0** | 上游 KMD/包（绑 `ACPI\SWOA0F01`，Lite 面板）在初代机上**没有设备可绑** | `WoA\winx\AcpiTables\Dsdt\Dsdt.asl:483` "no Tegra GPU engines are exposed" |
| 2 | 上游 WinX 集成走 Lite profile | `SWITCH_LITE_GPU / GPU_IRQ / GPU_POST` 与 HDH-001 构建绑定；初代机构建没有这些模块 | `tegra-x1-wddm/docs/PORTING.md:1159-1170` |
| 3 | 平台层假设 T214 | 资源、时钟/复位、中断、POST 显示接管、ACPI FAN0（x4 资源）都在 `platform/` 与 `integration/switch-lite/` | `PORTING.md:1180-1186` "For another board, start with the GM20B code under core/ ... The T214 assumptions are under platform/" |
| 4 | 电源桥接是 MAX77812 M1 | 初代机是 MAX77621 `0x1C`（6.25 mV），桥接/电压表全不同 | `Windows/pmic.h` + `SwitchFan` SPB 桥；本机 R364/Design-CPU-Clock |
| 5 | 512 MiB VidMm 上限 | 同为 4GB 机型问题不大，但上限值需在 T210 平台层重审 | `PORTING.md:1185-1186` |
| 6 | 显示 POST 归属 | Lite 把 POST framebuffer 交给 GPU0（`SWITCH_LITE_GPU_POST=1`）；初代机的显示链是 `winx-display` KMDOD | `PORTING.md:1167-1169` |

**可复用面（按价值排序）**：

- **NVK 核心 + GM20B 编码器**（mesa-switch `nouveau_horizon_gm20b.c`、命令/报告/围栏/CB 辅助）——GPU 硬件相同，基本可原样用；
- **WDDM 适配器骨架**：私有内存（VidMm）、提交/围栏、超时恢复、LUID/物理资源合约——与板级无关，可复用；
- **WSI 回退路径**（GDI 呈现）与 D3D UMD/Zink 的 WDDM 合约——在初代机上可以同样跑；
- **ACPI/资源模板**：FAN0 资源形状可参照；GPU0 需要为 T210 新写（见下）；
- **ACR/GR 固件**：GM20B 同款，32.7.6 签名大概率通用，需在 T210 上实测。

**"先上机"的最小现实路径**（也即目标里的验证顺序）：

1. 克隆上游源码（本地只有 docs）：`mesa-switch-woa`（WDDM/NVK Windows 侧）+ `mesa-switch`（NVK 核心）；本机无 GitHub 凭据，可由用户推送/拉取，或用 GitHub Actions 云端构建（参照诊断报告的建议）。
2. **建 T210 平台层**（对照 `tegra-x1-wddm/platform/` 的 T214 实现写一份 T210 版）：
   - 资源：`gpu@57000000` 的寄存器窗口（`socbase.dtsi:1395-1415`）、BAR1/窗口、`host1x` 引用；
   - 电源：MAX77621 `0x1C` 的 VDD_GPU（6.25 mV 步进）与开机电压；
   - 时钟/复位：`PLLGP` / `GPCCLK` / `reset-names="gpu"`；
   - 中断：T210 GPU 的 GIC SPI（对照 nouveau `nvkm/engine/device/tegra.c`，注意与 Lite 的 nonstall IRQ 方案不同）；
   - 固件：同上 32.7.6 GM20B；
   - 显示时序：初代机走 DSI/panel 参数（`tegra210-nx-common-disp.dtsi`），**不接管 POST**（先只做 render-only 适配器）。
3. **ACPI**：在 winx `Dsdt.asl` 增加可选 `GPU0` 设备（`ACPI\SWOA0F01`，默认 `_STA` 需能回落，避免影响现有系统），配套 UEFI 诊断（`SWITCH_*_GPU` 开关）。
4. **验证**：先跑"步骤 1"——在隔离分配里写哨兵 → 提交 GM20B report → 观察硬件围栏 → 回读校验；跑通后再 Vulkan 离屏 → 窗口呈现；DWM 适配另行评估（与目标一致）。
5. **风险点**：T210 GPU 中断/时钟树与 T214 不同、PLLGP 校准表不同、ACR 安全流程差异、MAX77621 的 DVC（若需要 DVS 必须处理）、`winx-display` 与 GPU0 的 POST 冲突——都必须先在步骤 1 前解决或显式绕开。

---

## 6. 表项补全清单（下一步要抄/要测的东西）

| 项 | 来源 | 状态 |
|---|---|---|
| T210 CPU OPP 电压（17 档 + OC 3 档，proc0/1） | L4T `tegra210-cpufreq.c`（gitlab switchroot，`l4t-kernel-4.9`） | 待抄录 |
| T210 GPU OPP 电压（12 档 × proc0/1/2） | L4T `tegra210-dvfs.c`（GPU DVFS tables） | 待抄录 |
| T210 EMC 每档 `emc-min-mv` / `gk20a-min-mv` | 本地 `tegra210-odin-emc.dtsi`（按档提取） | 已提取（见第 2 节） |
| 本机 CPU/GPU Speedo/process 实际值 | 本机 dmesg / fuse（Windows 侧需新写读取） | 待实测（已知 CPU Speedo 3 / proc 0） |
| 本机 EMC 稳定上限 | hekate `ram_oc` 爬坡（1600→1866→2000→2133） | 待实测 |
| 本机 GPU 稳定上限（>921.6 是否可行） | 电压爬坡 + 压力测试 | 待实测 |
| MAX77621 两路（0x1B/0x1C）读回确认 | 本机 I2C5 0x1B/0x1C | 待实测 |

---

## 7. 参考索引

- odin CPU/EMC/GPU 表：`WoA\ref\dts\kernel-dts\nx-platforms\tegra210-odin-common.dtsi`、`tegra210-odin-emc.dtsi`
- Lite 对照（T210B01）：`D:\paopao\wifi-ref\Windows-for-Switch-Drivers-hdh001\drivers\SwitchFan\src\t214\dfll_tables.h`、`emc_client_floor.h`
- Lite GPU 工程文档：`D:\paopao\WoA\mesa-switch-woa\tegra-x1-wddm\docs\{STATUS,DRIVER,PORTING}.md`
- L4T 内核源码（在线）：[switchroot/l4t-kernel-4.9](https://gitlab.com/switchroot/kernel/l4t-kernel-4.9) · [NVIDIA tegra210-core-dvfs.c](https://nv-tegra.nvidia.com/r/plugins/gitiles/linux-4.9/+blame/refs/heads/l4t/l4t-r32.2.3-4.9/drivers/soc/tegra/tegra210-core-dvfs.c)
- nouveau GM20B：`drivers/gpu/drm/nouveau/nvkm/{engine/device/tegra.c, engine/gr/gm20b.c, subdev/clk/gm20b.c}`
- 上游 GPU 代码仓库：[ruje0504-2/mesa-switch-woa](https://github.com/ruje0504-2/mesa-switch-woa) · NVK：[danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch)
- 本机 CPU 频率/超频记录：`WoA\CPU频率选项-20261109.md`
- 本机适配总报告：`WoA\诊断-WinX初代机适配.md`
