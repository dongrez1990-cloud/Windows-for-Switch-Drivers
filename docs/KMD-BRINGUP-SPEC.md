# KMD bring-up 规格（T210 / GM20B · 步骤 1 范围）

> 来源：上游 Lite 工程文档（`WoA\mesa-switch-woa\tegra-x1-wddm\docs\{DRIVER,PORTING,STATUS}.md`）+ 本地初代机证据（`T210.h`、`socbase.dtsi`）。
> 用途：作为 T210 平台层 + KMD 骨架的实现清单；**T214 的行号/数值只是参照，T210 要逐项替换**。

## 0. 范围与判定

- 本阶段（步骤 1）：KMD 初始化 GPU → 隔离分配写哨兵 → 提交 GM20B report → 硬件围栏 → CPU 回读校验。
- 通过标准（与 `STEP1-CHECKLIST.md` 一致）：设备 Code 0、频率/电压回读合理、围栏完成、哨兵被硬件改写、guards 完好。
- 不包含：Vulkan 绘制（步骤 2）、窗口呈现（步骤 3）、DWM（单独评估）。

## 1. 组件地图（Lite → T210 对应）

| 上游（Lite） | T210 工作 |
|---|---|
| `core/gr_firmware.c`（NETB/FECS/GPCCS 打包） | 直接复用（GM20B 固件格式相同） |
| `platform/t214_gr.c`（非安全 GR 引导） | 新增 `platform/t210_gr.c`（寄存器序列按 T210） |
| `platform/t214_acr.c` / `t214_pmu.c`（WPR/PMU/ACR 32.7.6） | 新增 `platform/t210_*.c`（安全流程需实测，先走 non-secure） |
| `platform/` 时钟/复位/电源 | 新增 `platform/t210/{clk,reset}.c` + `pmic_max77621.h` |
| `core/channel.c`（TSG zero/runlist/BAR1） | 复用逻辑（T210 的 USERD/runlist 偏移待核对） |
| `windows/`（WDDM 适配、UVP） | 复用骨架，按 T210 的 512 MiB 预算与资源重绑 |

## 2. 硬件初始化序列（T210，按顺序）

1. **资源映射**：GPU 0x57000000/0x58000000（各 16 MiB）、0x538f0000（4 KiB）、CAR 0x60006000、BAR1 别名映射。
2. **时钟/复位**（TODO 参照 nouveau `gm20b.c` / `clk-tegra210.c`）：PLLGP 锁相 → GPCCLK 起 384 MHz（保守）→ 放开 GPU reset → 读回验证非全 1。
3. **电源**：MAX77621 0x1C 按 OPP 表设压（`pmic_max77621.h`，回读校验 + DVC 同写）。
4. **固件准备**：`gr_firmware` 打包 NETB/FECS/GPCCS（校验选中 T21x DMEM 头格式；checksum 是 ABI 选择器不是签名）。
5. **GR 前导**（`gk20a_init_gr_prepare` 等价）：等 Falcon DMA IDLE、释放 PGRAPH/BLG/PERFMON、保留 FIFO/memory enable、设 GR 强制运行门控、两路时钟 RUN/电源 AUTO、开 GR FIFO/semaphore 访问。
6. **安全或非安全 GR**：先 non-secure（FECS/GPCCS DMA bootloader + FECS ready 握手）；安全路径（WPR/PMU/ACR 32.7.6）作为下一阶段，且必须在"WPR 已存在、不改安全 fuse、不把 WPR 映射进 CPU"的前提下做。
7. **FIFO/通道**：PBDMA0 经 `MC_ENABLE_PB`；runlist zero 绑定前阻塞、提交后允许、teardown 前再阻塞（照抄 Switchroot 调度控制）；TSG-zero 单成员布局（timeslice 128 @ scale 3）必须整段提交，不能只拷 `gk20a_get_ch_runlist_entry`。
8. **BAR1**：物理诊断路径走 `AccessedPhysically` 清 0 的 segment 2；USERD/物理 runlist 保持既有偏移与保护；BAR1 里发布 GP_PUT 只在绑定后。
9. **收尾**：FB/L2 同步、TLB invalidate，然后把控制权交给提交路径。

## 3. 内存与 UVP（VidMm 私有内存）

- 由 VidMm 管 backing pages 与 GPU VA；KMD 负责 4 KiB 页表构建与根页表（`SetRootPageTable` 等），**不**保留 VidMm 的 PTE 数组。
- 段 0 = resident system page；段 2 = 由 VidMm MDL PFN 解析（`TgGpuResolveVidMmPage`）。
- 应用中每个 GPFIFO 指令都以 **USER 权限**取指；ring/packet、GR 权限/补丁映射只读；RAMIN 与物理 runlist 不映射。
- 预算：**512 MiB 最大 GPU 工作集**（UMA 预算，动态分配）。初代机 4 GiB 内存下同值起步，后续实测再调。

## 4. 同步

- 步骤 1：**轮询围栏**（BAR1 别名读 GPU 写回的 fence 字），不依赖中断。
- 可选（IRQ 实验）：nonstall SPI 158 / GSI 190 + LIC bit30；Windows 拥有 GIC；只有 ISR+DPC 到达后第二个 report 才放行；teardown 要掩码/同步 ISR、取消 DPC、恢复 LIC 位。**T210 的 SPI/GSI 与 Lite 相同（157/158→189/190）**。

## 5. 诊断与失败隔离

- 关键失败点都要有 stage/register/value 记录（上游：stage 306/307/308/309/310；BAR1 phase；FECS CURRENT_CTX.VALID）。
- 围栏判定：哨兵被改写 + 期望值 + guards + 无 PBDMA/MMU 故障，四者同时成立。
- 失败时保留隔离分配与寄存器快照，不做自动重试，避免"沉默的坏状态"。

## 6. T214 → T210 差异清单（必须替换）

| 项 | Lite（T214） | 初代机（T210） |
|---|---|---|
| VDD_GPU | MAX77812 M1（SwitchFan SPB 桥） | MAX77621 0x1C，6.25 mV |
| 时钟树/校准 | C1 NA PLL / GPCCLK（T214 值） | T210 的 PLLGP 序列（nouveau） |
| GPU 频率上限 | 1228.8 MHz | 921.6 MHz（DTS retail regulator） |
| 内存 | LPDDR4X 训练表（B01） | LPDDR4 训练表（odin，1600 基准） |
| 平台层 | `platform/`（T214 假设） | `platform/t210/`（本目录） |
| ACPI | SWOA0F01（winx-gpu.patch） | `winx-gpu0.patch`（含 DSC 开关） |
| POST 显示 | 可接管 | 不接管（先 render-only） |

## 7. WDDM miniport 回调清单（参考 roskmd）

- `DriverEntry` → `AddDevice` → `StartDevice`（资源翻译：GPU 窗口、CAR、IRQ、I2C5 0x1C）→ `CreateDevice/Context` → `SubmitCommand` → `DdiQueryAdapterInfo` → UVP（根页表/段）→ `DestroyDevice`。
- Present/scanout 留到步骤 3；DWM 评估在步骤 4 之后。
- 内存预算、LUID 与物理设备匹配（NVK ICD 通过 LUID 关联）。

## 8. 参考（本地行号）

- 固件/ACR：`DRIVER.md:11-17, 116-158`
- GR 前导/阶段码：`DRIVER.md:100-114, 309-310`
- 通道/TSG/runlist：`DRIVER.md:44-53, 977, 517-531`
- BAR1/UVP/内存：`DRIVER.md:73-83, 179-258, 561-568, 1235-1238`
- IRQ/LIC：`DRIVER.md:627-649`
- WDDM 回调/预算：`PORTING.md:27-31`、`STATUS.md` 相关行
