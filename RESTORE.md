# Switch WoA GPU0 工作备份 —— 2026-11-21

这个备份的唯一目的：**在你回去做 SDR104/DDR200（SD 卡速度）实验之前，把"当前能正常工作的一整套"冻住**，
万一实验把机器搞到起不来，可以照着本文件回到今天这个状态。

## 1. 冻住的是什么（快照）

| 项 | 值 |
|---|---|
| 时间 | 2026-11-21 22:0x（本轮最后确认 22:12） |
| KMD | **0.1.0.112**，`oem97.inf`，`tegragpurender.sys` SHA256 前 32 位 `0C78DA6AD7A575B40D60209FFA4B2FBA` |
| 注册表开关 | **全部缺省**（无任何人工开关）：`HardwareStartLimit=5`、`PowerSkipMask=0`、`RgWrite=1`、`ClampNeedsTd=0`、`PriRing=1`、`RailSettleMs=25`、`BootGpuKhz=460800` |
| GPU 状态（已验证） | `SelfCheck=1`、`HardwareStatus=0`、`ClockResult=0`、`ExecutionResult=0`、`GrStage=9`、`measured=460800`、设备 `OK / problem=0` |
| 稳定性 | 460.8 MHz 连续 40 分钟 soak 零故障（71 个样本全绿） |
| 显示 | 面板 720×1280 竖屏存储 / 桌面 1280×720 / 路径 `ROTATE90` / vsync ≈63.7 Hz / 全屏上屏 ≈45 fps |
| 固件 | bl33 = **v72**（`E:\switchroot\winx-gpu0\bl33.bin`），启动项 `06-swoa-gpu0.ini`，hekate 自动启动指向 test 项 |
| 回滚镜像 | SD 上 `winx-gpu0\bl33-v5x/v6x/v67/v68-held.bin`（**本备份不含，仍在卡上**） |

目录内容：

```
docs\             STATUS-GPU0-BOOT.md（1–25 章全部结论与实测）、HANDOFF-20261120-GPU0.md 等
tools\            编 KMD / 打包签名 / 主机测试 / 基准 / 在线推送 / 安全检查 的 PowerShell 脚本
tests\            test_clock_t210_host.c、present_ref.c/h、screenbench.c、cpuprobe.c、soak.ps1
kmd-package\      0.1.0.112 的 sys+inf+cat 与固件 blob（可直接安装）
driver-src\t210\  T210 平台源码（power/clock/opp/frontend/gr/acr/pmu/dc… 全部）
driver-src\windows\ 本轮改过的 hardware.c / platform_adapter.c / render_miniport.c + present 相关
deploy-scripts\   改档位、等重启取数据、抗丢包远程命令、sshd 自愈
console-scripts\  控制台侧的 session 探针（1/2/3/4/5b/5c/5d/5e）、soak、屏幕安全复位
hekate-payload-copy\ 当时卡上 bootloader 目录的一份拷贝（hekate 6.5.4 payload + ini）
MANIFEST.sha256   全部文件的 SHA256（可用来验证备份完整性）
```

## 2. 出问题怎么回到这个状态

### A. 只是 GPU 驱动/开关的问题（还能进系统）

1. 想最保守：卡上双击 `F:\SwitchWOA\install-safe.cmd`（`HardwareStartLimit=1`、探针全关）。
2. 想回到今天这套：
   - 把 `kmd-package\` 三个文件拷到卡上 `F:\SwitchWOA\gpu-pkg\`，再跑卡上的
     `install-stack.cmd`（或从 PC 跑 `deploy-scripts\push-kmd-live.ps1 -Version 0.1.0.112 -Limit 5 -Mask 0x0 -RgWrite 1 -ClampNeedsTd 0 -BootGpuKhz 460800`）；
   - 或者干脆**删掉注册表里所有人工开关**（`HKLM\SYSTEM\CurrentControlSet\Services\tegragpurender\Parameters` 下的
     `HardwareStartLimit`/`PowerSkipMask`/`RgWrite`/`ClampNeedsTd`/`PriRing`/`RailSettleMs`/`BootGpuKhz`），
     让驱动走缺省值 —— 0.1.0.112 的缺省值**就是**今天这套工作档。
3. 验证：重启后读 `Parameters`，应见 `SelfCheck=1`、`GrStage=9`、`BootGpuMeasuredKhz=0x70800`（460800）。

### B. 进不了系统（黑屏/卡启动/反复重启）

1. 长按电源 12 秒关机 → 开机进 **hekate**。
2. 先试 **Tools → USB Mass Storage**，把 SD 挂到 PC：
   - 读 `F:\SwitchWOA\gpu-run.txt`：最后一个 `Step` 就是卡住的位置；
   - 离线改注册表：`WoA\erista-gpu\tools\set-gpu-safe.ps1`（把 `HardwareStartLimit` 写 0/1、`PowerSkipMask` 全跳过），
     这样驱动完全不碰 GPU，系统一定能起来。
3. 还不行就回退固件：把 SD 上 `winx-gpu0\bl33-v68-held.bin`（或 v67/v6x/v5x）复制成 `bl33.bin`。
4. hekate 里把自动启动改回 `00-swoa-v46.ini`（`switchroot/winx-v46` 那套**本轮从没动过**），确认能启动后再换回来。

### C. SDR104/DDR200 实验把读卡搞坏（你这个实验最可能踩的）

- **现象**：UEFI 阶段读卡失败 / 启动到一半掉卡 / Windows 里 SD 掉盘。
- **恢复**：SD 速度模式是 UEFI 侧（hekate/`SD` 初始化）的配置，通常不写进卡里，所以
  **换回旧的 `bl33.bin`（本备份之外的 v72 也在卡上）或改回默认速度档即可**；若卡本身被写坏，
  用 PC 读卡器 + 本备份恢复 `F:\SwitchWOA` 目录，再放回原 `bl33.bin`。
- **建议**：动手前先把卡上 `switchroot\winx-gpu0\` 整个目录拷到 PC（尤其 `bl33.bin` 与那几个 `bl33-v*.bin`），
  本备份里只有 bootloader 目录，没有 `bl33`。

## 3. 校验

```
# 在本目录执行（PowerShell）
Get-Content MANIFEST.sha256 | ForEach-Object {
  $h,$p = $_ -split '  ',2
  $got = (Get-FileHash $p -Algorithm SHA256).Hash
  if ($got -ne $h) { "MISMATCH $p" }
}
```

## 4. 已知的、不要太当真的东西（避免下次被误导）

- CPU/内存基准（`screenbench`、`cpuprobe`）在任何会话里都有效；
- **屏幕/合成的测量必须在交互会话（session 1）**跑：SSH 是 session 0，有一块假的 1024×768 虚拟显示；
- **接口会差 15 倍**：GDI+ 全屏 63–76 ms/帧，GDI `BitBlt` 0.17 ms 入队、+vsync 22 ms/帧（45 fps）；
- `DwmFlush` 节奏不等于刷新率（CPU 忙时从 46.9 掉到 19.9 fps），测刷新率用小窗口 vsync 限速法（≈63.7 Hz）；
- 驱动只暴露**一个模式一个方向**（1280×720 @ 90°），`ChangeDisplaySettingsEx` 对 720×1280 与 orient=0 都返回
  `DISP_CHANGE_BADMODE`，所以"改设置做旋转 A/B"在当前驱动下走不通。

## 5. 固件与启动项（本备份已包含，SDR104/DDR200 实验最需要的部分）

SD 卡在控制台上挂成 **`V:`**（`V:\` 就是 hekate/UEFI 那个 FAT 分区）。已从卡上拷出：

| 文件（`firmware\` 内） | 来源 | 大小 | SHA256（前 16 位） |
|---|---|---|---|
| `bl33-v72.bin` | `V:\switchroot\winx-gpu0\bl33.bin` | 1,048,576 B | `F766B207E89489F5`（与交接记录里的 v72 完全一致 ✓） |
| `bl33-v68-held.bin` | `V:\switchroot\winx-gpu0\bl33-v68-held.bin` | 1,048,576 B | `8E0987C7DCAD7BAC` |
| `hekate_ipl.ini` | `V:\bootloader\hekate_ipl.ini` | 132 B | `FB9A1F2221218F31` |
| `startup.nsh` | `V:\startup.nsh` | 410 B | `A0EB4EFB2A891333` |

`hekate_ipl.ini` 当前内容（这就是"自动启动到 test 项"的设置）：

```ini
[config]
autoboot=6          ; 第 6 项 = 06-swoa-gpu0.ini（GPU0 测试项）
autoboot_list=1
bootwait=2
backlight=100
noticker=0
autohosoff=1
autonogc=1
updater2p=1
bootprotect=0
```

卡上 `V:\switchroot\winx-gpu0\` 里还有 16 个历史回滚镜像（`bl33-v49/v50/v51-held/…/v67-held/v68-held`），
**本备份只带了当前 v72 和 v68-held 两个**，其余仍在卡上。

### SDR104 / DDR200 实验失败后的恢复

1. **卡还能被 PC 读**（最常见）：把 `firmware\bl33-v72.bin` 覆盖回卡上
   `switchroot\winx-gpu0\bl33.bin`，把 `firmware\hekate_ipl.ini` 覆盖回 `bootloader\hekate_ipl.ini`
   → 回到今天这个固件与自动启动设置。
2. 如果新固件本身有问题、v72 也救不回：用 `firmware\bl33-v68-held.bin` 改名成 `bl33.bin`（更旧的已知可用固件）。
3. 如果 hekate 自动启动停不下来（一开机就进坏固件）：把 `hekate_ipl.ini` 的 `autoboot` 改成 `0`，
   或开机时按住 `-` 音量键进 hekate 菜单另选启动项。
4. 如果卡彻底读不了：换一张卡，把本备份的 `firmware\` 与卡上其它目录恢复过去（Windows 分区需另做镜像）。

## 6. 从 GitHub 取回这份备份

```powershell
git clone -b backup/20261121-gpu0 git@github.com:dongrez1990-cloud/Windows-for-Switch-Drivers.git woa-gpu0-backup
# 或 HTTPS：
git clone -b backup/20261121-gpu0 https://github.com/dongrez1990-cloud/Windows-for-Switch-Drivers.git woa-gpu0-backup
```

仓库：`dongrez1990-cloud/Windows-for-Switch-Drivers`，分支 **`backup/20261121-gpu0`**
（与仓库其它分支相互独立，不影响 `erista-0.7.x` / `uefi/erista` 等）。
