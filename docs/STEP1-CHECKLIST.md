# 步骤 1 检查清单 —— GPU 执行与回读（初代机 T210）

> 前置条件：`woa\winx` 以 `-D SWITCH_GPU0=1` 构建，刷入 ESP，重启进入 Windows；Windows 侧安装匹配 `SWOA0F01` 的 KMD 包。
> 回滚：`_STA` 开关 / 卸载驱动包 / 恢复旧 bl33（不改任何分区）。

## A. 设备枚举（在 Windows 内）

```
# 设备是否存在（注册表直读，避免 WMI 慢查询）
reg query HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SWOA0F01
reg query HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SWOA0F01\0 /v Driver

# 显示类设备
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}" /s /v DriverDesc | findstr /i "GM20B Tegra X1"
```

通过标准：`SWOA0F01\0` 存在，Driver 指向我们的 KMD；`DriverDesc` 出现 GM20B / Tegra X1。

## B. 驱动状态（Lite 验收口径）

| 项 | 期望 |
|---|---|
| 设备状态 | Code 0（与 Lite `NATIVE_STARTUP_RESULT.md:14` 同口径） |
| GPU 频率回读 | GPCCLK 计数器 ≥ 384 MHz（T210 起步 307.2/384） |
| 电源回读 | MAX77621 0x1C VOUT 与 OPP 表一致（见 `ERISTA-DVFS-TABLES.md` §4） |
| 错误 | 无 PBDMA/MMU 故障、无超时恢复 |

## C. 执行与回读（核心验收）

1. 准备 1 MiB 隔离分配（VidMm 私有内存），哨兵写 `0x5A5A0001`；
2. 提交 GM20B report（写入不同值，例如 `0x12345678`）；
3. 轮询 GPU 内存里的 fence 字（BAR1 可读别名），等待硬件完成；
4. CPU 回读：期望 fence 完成 + 哨兵 = `0x12345678`（或报告约定的值），前后 guard 字完好；
5. 重复一次，换新值，确认不是残留。

**判定：四个条件同时成立才算通过**：设备枚举 OK、频率/电压回读合理、fence 完成、回读值正确且 guards 完好。设备出现不等于执行通过。

## D. 记录模板

```
日期：
固件 bl33 sha256：
GPU0 ACPI：_STA=0x0F，blob sha256：
驱动包：版本 / INF / SYS sha256：
设备状态：Code ?
GPCCLK kHz：          MAX77621 0x1C mV：
哨兵：写入 0x5A5A0001 → 回读 ?
fence：完成? 值?
错误：无/…
回滚状态：已保留
```

## E. SSH 探测脚本

`tools/check-gpu-baseline.ps1`（在本机运行，经 SSH 到 Switch）：

```powershell
powershell -NoProfile -File D:\paopao\WoA\erista-gpu\tools\check-gpu-baseline.ps1
```

输出：SWOA0F01 是否存在、显示类设备清单（注册表直读，避免 WMI 挂起）。
