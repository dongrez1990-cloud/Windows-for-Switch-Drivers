# T210 时钟层移植说明（GPCPLL / PLLP_OUT5 / GPCCLK）

> 依据：上游 `tegra-x1-wddm/platform/t214_clock.[ch]`（真实源码，已克隆）+ L4T `tegra210-dvfs.c`/`cvb.c`/`clk-tegra210.c`（已提取到 `src/l4t-kernel/extracted/`）。
> GM20B 的 GPCPLL 寄存器块在 T210 与 T210B01 上是**同一套**（都在 GPU 孔径 `0x57000000` 内），T214 的代码可以按"换表 + 换电源 + 换板检"移植。

## 1. 寄存器映射（t214_clock.c 实测，T210 共用待上机验证）

| 名称 | 地址 | 用途 |
|---|---|---|
| CFG | `0x57000000 + 0x137000` | GPCPLL 控制/使能 |
| COEFF | `+0x137004` | M/N/PL 系数 |
| CFG2 / CFG3 | `+0x13700c` / `+0x137018` | 分频/采样 |
| DVFS0 / DVFS1 / DVFS2 | `+0x137010` / `+0x137014` / `+0x132820` | DVFS 参数 |
| VCO | `+0x137100` | VCO 选择/分频 |
| OUT | `+0x137250` | 输出分频 |
| BYPASS | `+0x137340` | 旁路 |
| SLOW | `CAR + 0x020160` | 慢速路径 |
| THERM | `CAR + 0x020798` | 限频掩码（mux 切换时临时清 0） |
| SVOP / SVOP_OVERRIDE | `CAR + 0x021944/8` | SVOP 覆盖 |
| COUNT_CFG / COUNT_VAL | `+0x134124` / `+0x134128` | GPCCLK 频率计数器（真实回读） |

CAR 基址 `0x60006000`；clock 184 = GPU gate，clock 189 = PLL_G_REF；PLLP 在 CAR+0x0a0；PLLP_OUT5 在 CAR+0x67c（bring-up 用 204 MHz 输出）。

## 2. T210 需要的改动（相对 t214_clock.c）

1. **板检**：HIDREV `0x70000804 & 0xfff0 == 0x2110`（Erista）；B01 是 `0x2120`。
2. **电压表**：把 `TG_GPU_HIOPT_*`（B01 16 档）换成 T210 的 CVB 表——已提取到
   `src/l4t-kernel/extracted/gpu-dvfs-tables.txt`：
   - `NA_FREQ_CVB_TABLE`（768～998.4 MHz）、`NA_FREQ_CVB_TABLE_XA`（768～844.8）、`FIXED_FREQ_CVB_TABLE`（768～921.6），格式 `{freq, {}, {c0,c1,c2,c3,c4,c5}}`
   - `gpu_cvb_dvfs_table`：speedo 4 → `pll_min_millivolts=918, max_mv=1113, max_freq=844800`（retail 上限）；speedo 3 → `pll_min_millivolts=810`（完整条目见文件）
3. **CVB 公式**：`cvb.c` 的 `tegra_get_cvb_voltage()`：
   `mv = round(round(c2*speedo/s_scale)*speedo/s_scale + c1*speedo/s_scale + c0)`，
   再按 `voltage_scale`/`rail_alignment` 取整（MAX77621 步进 6.25 mV，offset 606.25 mV）。
   本机带温补时用 `tegra_get_cvb_t_voltage()`（c3/c4/c5）。
4. **电源**：MAX77621 `0x1C`（`pmic_max77621.h` / `t210_power.c`），而非 MAX77812 M1。
5. **频率档位**：T210 retail 默认 76.8–844.8 MHz（DTS `gpu-dvfs-rework` 可到 921.6）；GPCPLL 的 N/PL 对可沿用 t214 表的同频对（76800→N=36,PL=9；153600→40,5；230400→36,3；307200→48,3；384000→40,2；460800→48,2；537600→56,2；614400→64,2；691200→36,1；768000→40,1；844800→44,1；921600→48,1——VCO 38.4 MHz×N ∈ 1300–2600 MHz 约束一致）。
6. **CPU 侧**：T210 CPU FV 表（`CPU_FV_TABLE`，已提取）：
   `51000→850, 102000→850, 204000→850, 307200→850, 403200→850, 518400→850, 614400→868, 710400→912, 825600→962, 921600→1006, 1036800→1062, 1132800→1118, 1228800→1168`（mV，speedo 5/3/2 各档 min/max 见文件）。

## 3. 待验证/开放项

- GPCPLL 的 T210 上电序列（NV_PGPC_PLL 门控/旁路/锁定）需按 nouveau `nvkm/subdev/clk/gm20b.c` 与 switchroot nvgpu `clk_gm20b.c` 复核；
- C1 NA PLL 是否 T210 可用（t214 注释说 B01 要求该路径）——T210 可能走相同的 NA 路径，需上机确认；
- 热区段（thermal range）与 EMC 下限对 T210 的取值；
- 每台机 Speedo/process 实测（见 `ERISTA-DVFS-TABLES.md` §0）。
