# SwitchWifi 0.7.2 基线快照（2026-11-18 冻结）

> 目的：在"按 GitHub（Lite 0.5.7）重构 WiFi 驱动"之前，把**我们现有的 0.7.2 原样留住**。
> 重构失败 / 上机炸了 → 直接从这里恢复，不需要翻会话记录。

## 一、这份快照里有什么

| 路径 | 内容 |
|---|---|
| `source\SwitchWifi\` | 0.7.2 驱动**完整源码树**（windows / core / package / tests，80 文件） |
| `package-072-staged\` | 0.7.2 成品驱动包：`switchwifi.sys` / `.inf`(0.7.2.0) / `.cat`（11-15 构建，已签名） |
| `tools\Build-SwitchWifi.ps1` | WiFi 专用构建脚本（原未跟踪，已随手备份） |
| `tools\SwitchWOA-Test.cer` | 测试签名证书 |
| `tools\uncommitted-0.5.7-to-0.7.2.patch` | 0.5.7→0.7.2 全量 diff（163 KB，`git diff --binary`） |
| `MANIFEST-sha256.txt` | 全部文件的 SHA256 / 大小 / 相对路径 |

## 二、Git 锚点

```
仓库   D:\paopao\WoA\Windows-for-Switch-Drivers
分支   hdh001-winarm-bounded-write
提交   705901b  "SwitchWifi 0.5.8-0.7.2 WIP (Erista) - checkpoint before GitHub-driven rebuild"
Tag    wifi-0.7.2-checkpoint
父提交 e220195  "Enable autonomous BQ24193 charge termination on Lite"（11-08 原 HEAD）
```

> 该提交**只在本地**，没有 push。它把 22 个未提交改动（19 已跟踪 + 3 未跟踪）一次性固化，
> 从此 0.7.2 随时可用 `git diff wifi-0.7.2-checkpoint -- <path>` 对账，或
> `git checkout wifi-0.7.2-checkpoint -- drivers/SwitchWifi` 单目录回滚。

## 三、恢复办法

**回滚源码到 0.7.2（重构版本不满意时）：**
```powershell
cd D:\paopao\WoA\Windows-for-Switch-Drivers
git checkout wifi-0.7.2-checkpoint -- drivers/SwitchWifi
```

**纯文件恢复（仓库被清理也不怕）：**
```powershell
robocopy D:\paopao\baseline-20261118-wifi072\source\SwitchWifi `
         D:\paopao\WoA\Windows-for-Switch-Drivers\drivers\SwitchWifi /MIR
```

**回滚机器上的驱动**（注意铁律：`pnputil` 只升不降，回退要用更高 DriverVer 重新签名打包）：
```powershell
# 现成脚本：D:\paopao\switchjoycon\tools\pack-029-as-036.ps1 改参数即可
# 或用本快照的 package-072-staged 重新打一个 0.7.3.x 的包去"向下覆盖"
```

## 四、0.7.2 的已知状态（重构的出发点）

- 版本：INF `DriverVer=11/12/2026,0.7.2.0`；机器上实际跑的是 **0.7.0.0 (oem13.inf)**，比源码旧一档；
- 断连策略是**绕法**：`join.c` 的 `JoinTimeout()` 2 秒到点后无论 `LinkIsDown()` 结果如何，
  一律 `Finish(Leave)` → `FirmwareDirty = TRUE` → `JoinReset()` 把 `Poisoned` 清成 FALSE，
  靠"下次连接自己重置"蒙混 → **掉线后必须禁用/启用网卡才能重连的根因**；
- `ConfirmLinkDown()` / `SWIFI_LEAVE_PHASE` / `SwifiReportLeave()` 在 0.7.2 中**均不存在**（各 0 处）。

重构目标见 `D:\paopao\wifi-ref\WIFI-HANDOFF.md` 第七节：把 Lite 0.5.7 的
`ConfirmLinkDown()` + 4 阶段 leave 上报搬进来，删掉 `FirmwareDirty` 绕法。

## 五、关键哈希（核对快照完整性）

```
switchwifi.sys  (0.7.2, 11-15)  SHA256 = 56F77108CA5B860B...
switchwifi.inf  (0.7.2.0)       SHA256 = EC4766B99FBD3E5F...
```
