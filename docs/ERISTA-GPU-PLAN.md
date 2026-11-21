# 初代机（Erista / T210）GPU 硬件加速 · 实施计划

> 目标：`goal-f5f0b435-65d1-4a21-aa8f-87844b217a52`。
> 本文是"看上游 GPU 驱动能不能先上机"的结论落地：**不能直接装，但软件可复用**；本文件给出 T210 平台层设计、GPU0 ACPI 草稿、步骤验证顺序与源码获取方案。
> 相关：`ERISTA-DVFS-TABLES.md`（频率/电压表）、`gpu0-erista.asl`（ACPI 草稿）。

---

## 0. 资产与缺口

| 资产 | 位置 | 状态 |
|---|---|---|
| 目标与验证顺序 | 本会话 goal | 已建立 |
| T210 CPU/EMC/GPU 表 | `D:\paopao\WoA\erista-gpu\ERISTA-DVFS-TABLES.md` | 已建，部分电压待抄 |
| 上游 Lite GPU 工程文档（16 篇） | `D:\paopao\WoA\mesa-switch-woa\tegra-x1-wddm\docs\` | 本地可读 |
| GPU0 资源契约（apertures/IRQ/LIC/固件） | `WoA\ref\socbase.dtsi:1395-1409` + 上述 docs | 已核实 |
| 初代机 UEFI（winx `uefi/erista`） | `D:\paopao\WoA\winx`（分支 `uefi/erista`，HEAD `e6e04f3`） | 可用，**无 GPU0** |
| 初代机 DSDT | `WoA\winx\AcpiTables\Dsdt\Dsdt.asl` | 有 FAN0/CPUC/I2C5 等，无 GPU 设备 |
| 驱动栈（风扇/电源/电池/音频/触摸/蓝牙） | 机器上已装（switchfan 1.0.0.22 等） | 可用；与 GPU 无直接耦合 |
| **构建环境** | LLVM 23.1.5（`C:\Program Files\LLVM\bin`：clang/clang-cl/lld/lld-link/llvm-readobj）；devkitA64（gcc 16.1.0 + devkitPro msys2 make）；Windows Kits 10（含 WDK `km` 头）；`iasl`（`WoA\build\iasl\Windows-x86\iasl.exe`） | ✅ 已核实 |
| **SSH** | `ssh -i D:\paopao\ssh\woa_ed25519 woa@192.168.1.74`（Switch 主机，hostname=SWITCH） | ✅ 可达；⚠️ WMI 重查询会挂 sshd（`DshSshdWatchdog` 自愈） |
| **缺口 1：mesa-switch-woa 源码** | ✅ 已克隆到 `erista-gpu\src\mesa-switch-woa`（485 MB，含 `tegra-x1-wddm/{core,platform,windows,integration}`） | 可用 |
| **缺口 2：mesa-switch（NVK）源码** | 克隆中（GitHub 网络不稳，稀疏克隆重试） | 进行中 |
| **缺口 3：外网** | HTTPS 直连被沙箱 schannel 挡住；**Git 用 `-c http.sslBackend=openssl` 可通** | 已解决 |
| KMD bring-up 规格（实现清单） | `KMD-BRINGUP-SPEC.md` | 已建 |

**构建证据（已实测）**：`clang --target=aarch64-w64-windows-gnu -std=c11 -ffreestanding -O2` 编译 `platform/t210/pmic_max77621.h` 的测试 TU → `llvm-readobj` 报告 `COFF-ARM64` / `IMAGE_FILE_MACHINE_ARM64`，与上游 Lite 的构建证据同口径；`aarch64-none-elf-gcc` 也能把 `tests/test-pmic-max77621.c` 编成 ARM64 ELF（不可本地运行，无 qemu）。

---

## 1. GPU（GM20B）资源契约 —— T210 与 Lite 相同

来源：`WoA\ref\socbase.dtsi:1395-1409`（T210 SoC 基座）与上游 Lite 文档（STATUS.md `0.1.0.101` / DRIVER.md `.32`）。

| 资源 | T210（初代机） | 说明 |
|---|---|---|
| GPU 窗口 A | `0x57000000`，16 MiB | gr/引擎寄存器窗口 |
| GPU 窗口 B | `0x58000000`，16 MiB | 辅助窗口 |
| GPU 杂项 | `0x538f0000`，4 KiB | |
| 中断 stall / nonstall | GIC SPI **157 / 158** → ACPI GSI **189 (0xBD) / 190 (0xBE)** | Lite 文档实测 nonstall=GSI 190 |
| 时钟/复位 | CAR `0x60006000`（TEGRA210_CLK_GPU / reset "gpu"） | T210 与 Lite 寄存器块同名不同值 |
| LIC | `0x60004414 / 0x1C`（bank 4，仅 IRQ 实验模式需要） | Lite `SWITCH_LITE_GPU_IRQ=1` 只加这一段 |
| 电源 | **MAX77621 I2C5 `0x1C`，6.25 mV 步进** | Lite 是 MAX77812 M1（SwitchFan SPB 桥） |
| 固件 | GR：`NETB_img.bin / fecs.bin / gpccs.bin`（tegra21x/gm20b）；ACR：NVIDIA **32.7.6** 六件套 | GM20B 相同，可复用（T210 安全流程需验证） |
| SMMU / host1x | `iommus = <&smmu TEGRA_SWGROUP_GPUB>`，host1x `0x50000000` | T210 GPU 走 IOMMU |

> 注意：T210 GPU 在 DTS 里 `status = "disabled"`，由驱动打开；ACPI 设备只声明资源，不负责上电/复位。

---

## 2. 板级差异 → 平台层工作项（对照 Lite 实现）

| # | 项 | Lite（T214/B01） | 初代机（T210） | 工作项 |
|---|---|---|---|---|
| 1 | VDD_GPU | MAX77812 M1（SwitchFan SPB 桥） | **MAX77621 0x1C**（6.25 mV） | 写 `platform/t210/pmic.c`：读/写 VOUT + DVC，起步电压取 ODN 表 |
| 2 | 时钟/复位 | T214 平台层 | T210 `PLLGP/GPCCLK` + `CLK_GPU` reset | 写 `platform/t210/clk.c`；C1 NA PLL 校准表按 T210 改 |
| 3 | 中断 | nonstall SPI158/GSI190 + LIC bit30 | 相同 SPI/GSI（均 157/158） | 先不做 IRQ（默认渲染路径用轮询围栏），IRQ 作为可选步骤 |
| 4 | POST 显示 | 可接管 POST（`SWITCH_LITE_GPU_POST=1`） | **不接管**：保持 `winx-display` KMDOD | render-only 适配器（第一步）；POST 接管留到窗口呈现阶段 |
| 5 | ACPI | 设备 `SWOA0F01`（winx-gpu.patch） | 需新增 `GPU0` 设备 | 用 `gpu0-erista.asl` 草稿，默认关闭 |
| 6 | 内存上限 | VidMm 512 MiB 上限 | 4 GB 机型同问题 | 保留上限，平台层改为可配 |
| 7 | 固件加载 | 安全 ACR（T214） | T210 走 non-secure GR 或 ACR（需实测） | 先在 non-secure 路径验证 GR，再评估 ACR |
| 8 | SMMU | 需要 IOMMU 映射 | 同 | 参照 Lite `gpu_va`/SMMU 代码 |

---

## 3. 实施步骤（对应目标验证顺序）

**Step 0 · 源码与构建环境**（用户执行/授权）
- 获取 `mesa-switch-woa` + `mesa-switch` 源码（第 6 节）；
- 构建：GitHub Actions（云端）或具备 WDK 的 Windows 机器。

**Step 1 · T210 平台层**（本地可先写）
- 目录：`tegra-x1-wddm/platform/t210/`（`clk.c`/`pmic.c`/`reset.c`/`firmware.c`/`display.c` 骨架）+ `integration/switch-erista/`；
- 实现依据：`KMD-BRINGUP-SPEC.md`（硬件初始化序列、UVP 内存契约、同步、T214→T210 差异清单）；
- 已落盘底稿：`platform/t210/pmic_max77621.h`（电压公式/投票/回读，公式已由 winx R364/R365 与编译期断言双重验证）、`platform/t210/pmic.c`（Windows I2C 电源层，x64/ARM64 双目标编译通过）、**`platform/t210/t210_power.[ch]`（T210 上电/下电/复位/MC drain 序列，改编自上游 `t214_power.c`；HIDREV 0x211x 校验 + MAX77621 0x1C + 6.25 mV 电压；clang/gcc 双编译器通过）**、`clk.h`/`reset.h`、`platform/t210/README.md`（平台层契约）、`STEP1-CHECKLIST.md`（步骤 1 验证清单）、`tools/check-gpu-baseline.ps1`（SSH 基线探测）；
- 本地编译验证：`clang --target=aarch64-w64-windows-gnu` 与 `aarch64-none-elf-gcc` 均可编译平台层头文件 TU（0 错误）；WDDM DDI 头在 `Windows Kits\10\Include\10.0.26100.0\shared\`（d3dkmddi.h 等）齐全；
- 冒烟：`TegraGpuCore.lib` 能在 T210 资源上初始化（无 GPU 执行）。

**Step 2 · ACPI GPU0 + 枚举**
- 补丁已生成：`winx-gpu0.patch`（`Dsdt.asl` +28 行、`NintendoSwitch.dsc` +6 行；`git apply --check` 通过）；
- 接线已完成：DSC 的 `SWITCH_GPU0` → `-DSWITCH_GPU0=1` 进 `AARCH64_ASLPP_FLAGS`，构建时用 `-D SWITCH_GPU0=1` 显式开启；默认不生效。
- 目标：设备管理器出现 `ACPI\SWOA0F01\0`（Tegra X1 GM20B），Code 0；
- 回滚：构建开关 / 卸载驱动包 / 恢复旧 bl33。

**Step 3 · 步骤 1：真实 GPU 执行与回读**
- 隔离分配写哨兵 → 提交 GM20B report → 等硬件围栏 → 回读校验（与 Lite `.30` 的 `--gpu-729` 同型）；
- 通过标准：哨兵值被硬件改写、围栏完成、CPU 回读一致、无 PBDMA/MMU 故障。

**Step 4 · Vulkan 离屏**
- ARM64 NVK ICD（`libvulkan_nouveau.dll`）注册 `nvk_windows_icd.json`，画出已知三角形并与期望像素比对。

**Step 5 · 窗口呈现**
- `VK_KHR_win32_surface` + swapchain；先接受 GDI 回退路径（上游已知）；
- 与 `winx-display` 的 present-allocation/fence 契约对齐后再谈 GPU 呈现。

**Step 6 · DWM 评估**（单独）
- 只有到这一步才评估 D3D→Vulkan UMD / Zink 给 DWM 用；不能把 Vulkan 跑通当作桌面已加速（目标明确要求）。

---

## 4. GPU0 ACPI 草稿

见同目录 `gpu0-erista.asl`。要点：

- `Device (GPU0)`，`_HID = "SWOA0F01"`，`_UID = 0`；
- `_CRS`：双 16 MiB 窗口 + `0x538f0000` 4 KiB + CAR `0x60006000` + GSI `0xBD/0xBE` + `I2cSerialBusV2 (0x001C, … "\\_SB.I2C5")`（KMD 直接驱动 MAX77621）；
- 默认 `_STA` 返回 0x0F 但**仅当编译开关 `SWITCH_GPU0` 打开**；否则设备不存在，对现有系统零影响；
- IRQ 实验时再追加 `0x60004414/0x1C`（LIC bank 4）。

---

## 5. 验证与回滚纪律

- 设备枚举/回读每步都要有"失败隔离"：哨兵值、围栏、guards，不把"设备出现"当"GPU 执行通过"；
- 显示链保持双轨：`WINX0001`（DISP）不动，GPU0 只在 render-only 场景出现；
- 回滚：`_STA`→0 / 编译开关 / 卸载驱动包；固件回滚用现有启动项。
- 风扇/电源/电池等现有驱动不重编（与 Lite 的 NVDEC 耦合无关，本线已验证 0 处引用）。

---

## 6. 源码获取方案（现实约束：本机 HTTPS 全不通）

**方案 A · 本机 clone（现在可行！）**：HTTPS 直连被沙箱的 schannel 挡住，但 **Git 的 OpenSSL 后端可用**——逐条命令带 `-c http.sslBackend=openssl` 即可连通 GitHub（`git ls-remote https://github.com/danfromtico/mesa-switch HEAD` 已成功）。`git config --global` 写用户目录被沙箱拒绝，因此用逐命令参数。
```powershell
git -c http.sslBackend=openssl clone --depth 1 https://github.com/ruje0504-2/mesa-switch-woa.git D:\paopao\erista-gpu\src\mesa-switch-woa
git -c http.sslBackend=openssl clone --depth 1 https://github.com/danfromtico/mesa-switch.git D:\paopao\erista-gpu\src\mesa-switch
```
克隆已在本会话启动（后台任务），落盘后即可按真实 ABI 写 T210 平台层。

**方案 B · GitHub Actions 云端构建**（可行，需用户 GitHub 账号）
- fork 两个仓库 → 加 workflow：Windows runner（`windows-2022` + WDK NuGet）编译 KMD/ICD/测试 → 产物 artifact 下载；
- 本机已有 LLVM+WDK 头，产物也可下载后本地复验。

**方案 C · 用户手动放置源码**（推荐，最快）
- 把两个仓库解压到 `D:\paopao\erista-gpu\src\mesa-switch-woa\` 与 `D:\paopao\erista-gpu\src\mesa-switch\`；
- 我用本机 LLVM/iasl/devkitA64 直接改 T210 平台层并编译验证（含 `aarch64-w64-windows-gnu` 目标）。

**KMD 构建的两条本机限制（实测结论）**
1. **缺 `dxgkrnl.lib`**：WDK 的 `Lib\10.0.26100.0\km\arm64` 有全套库（含 `vhfkm.lib`）与 `Lib\wdf\kmdf\arm64\1.35\{wdfldr,wdfdriverentry}.lib`，但**没有 `dxgkrnl.lib`**（全盘搜索无）；WDDM miniport 的 `DxgkInitialize` 无法本地链接。
2. **WDDM 共享头在 clang-cl 下需要用户态类型前导**（`UINT`/`DWORD`/`BOOL`/`POINT`/`RECT`/`MAX_PATH` 等，且 `d3dkmddi.h` 个别声明仍解析失败）。KMDF/WDM 不受影响（已验证可编可链可签）。
3. 对策：KMD 走 Actions（完整 WDK）构建，或补装 WDK 的 display 组件/复制 `dxgkrnl.lib`；本机继续负责平台层代码、固件（iasl/devkitA64）、测试与签名。

**固件（EDK2）构建的本机限制（实测结论）**
- 本机缺可用的 **python**（WindowsApps 是商店 stub；msys2 python 被沙箱命名管道限制拦截）、**make** 与 **宿主 x64 编译器**（msys2 运行时同样被拦）；EDK2 BaseTools/`build.py`/`make-bl33.py` 都依赖它们。
- devkitA64 的 `aarch64-none-elf-gcc` 能跑（可做交叉编译），但 `GCC5_AARCH64_PREFIX=aarch64-linux-gnu-` 的宿主工具链不齐。
- 对策：固件在用户环境或 GitHub Actions（`ubuntu-latest` + `gcc-aarch64-linux-gnu` + python）构建；本机负责改 DSC/ASL、跑 iasl 语法检查、审阅 diff。

---

## 7. 风险清单

- **PLLGP/GPCCLK 校准**：T210 与 T214 不同，C1 NA PLL 表必须按 T210 重做（GM20B nouveau `subdev/clk/gm20b.c` 可参考）；
- **ACR 安全启动**：T210 上 32.7.6 固件签名是否可用，需实测；备选 non-secure GR；
- **POST 冲突**：`winx-display` 与 GPU0 的 POST 归属未定，先 render-only 绕开；
- **MAX77621 DVC**：若走 DVS 需要处理 DVC（上游 Lite 是 M1 直写）；
- **内存上限 512 MiB**：4 GB 机型要重评（属于 VidMm 平台层）；
- **显示时序**：DSI/面板参数在 `tegra210-nx-common-disp.dtsi`，窗口呈现阶段再碰。

---

## 8. 下一步待办（按序）

1. 【等用户】选第 6 节方案，把源码/构建环境准备好；
2. 【我】按第 2 节表写 `platform/t210` 骨架（先 `pmic.c` + `clk.c` + `reset.c`）；
3. 【我】把 `gpu0-erista.asl` 合入 winx Dsdt（`-D SWITCH_GPU0`）+ 加 UEFI 诊断开关；
4. 【我】补表：L4T CPU/GPU OPP 电压列（需能上网的机器），以及本机 Speedo 实测；
5. 【上机】Step 2 → Step 3 逐步验证。
