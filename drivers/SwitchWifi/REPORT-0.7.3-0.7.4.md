# WiFi 驱动重构 — 0.7.3.0 / 0.7.4.0 落地报告（2026-11-18 下半场）

> 接 `WIFI-HANDOFF.md`（上半场交接）。本文记录：**保留 0.7.2 → 按 GitHub（Lite 0.5.7）重构 →
> 构建 → 上机测试 → 找到并修掉真正卡住重连的缺陷** 的完整过程与证据。

## 一、三十秒结论

1. **0.7.2 已固化，随时可回退**：本地 commit `705901b` + tag `wifi-0.7.2-checkpoint`，
   另有文件级快照 `D:\paopao\baseline-20261118-wifi072\`（含 0.7.2 源码树、成品包、构建脚本、
   SHA256 清单）。见该目录 `BASELINE.md`。
2. **按 GitHub 重构完成并上机**：0.7.3.0 搬进 Lite 0.5.7 的断连策略（`ConfirmLinkDown` +
   4 阶段 leave 上报 + 删掉"2 秒到点无条件判死/强清中毒"的绕法）。
3. **测试推翻了原判断**：0.7.3.0 上机后**设备再也不会"中毒"**（连做 5 次断连，`Status` 恒 OK、
   `ProblemCode` 空），但**链路仍然回不来**。查 WLAN-AutoConfig 日志发现是
   **关联成功、4 次握手超时**（`11006 无线安全失败：动态密钥未在配置时间范围内成功`）。
   → 也就是说"必须禁用/启用网卡"的**真正根因不在断连策略，而在重连后的 EAPOL 接收路径**。
4. **根因定位并修复（0.7.4.0）**：`station_data.c` 的 `RxComplete()` 用**空缓冲区投递时**记下的
   epoch 判断收到的包是否属于当前关联。重连后，EAPOL M1 落在上一次关联投递的旧缓冲区上，
   被当成过期包**丢弃** → 4 次握手永远起不来。改为**按到达时刻**记 epoch（与 Lite 参考实现一致），
   队列里的跨界包仍由 `AdvanceRx` 拦截。
5. **上机验收通过**：0.7.4.0 连续 5 次"断开→重连"**全部约 2 秒内自动恢复**（702–866.5 Mbps），
   **全程不禁用/启用网卡、不重启**，`erx`(EAPOL 帧) 3→17、`kadd`(装密钥) 2→12 逐轮递增
   （0.7.3.0 时这两个数一直冻在 3 和 2）。

## 二、版本与产物对照

| 版本 | 内容 | 产物 |
|---|---|---|
| 0.7.2.0 | 重构前基线（**保留**）| `baseline-20261118-wifi072\package-072-staged\`；`build\SwitchWifi072-rebuild\` |
| 0.7.3.0 | 移植 Lite 0.5.7 断连策略 | `build\SwitchWifi073\` |
| 0.7.4.0 | + 修 EAPOL 到达 epoch（**当前机器上运行**）| `build\SwitchWifi074\`（`switchwifi.sys` 179112 B）|
| 0.7.9.0 | **回退包**：0.7.0.0 原始二进制，仅 DriverVer 抬高后重签 | 机器 `C:\SwitchWOA\drivers\switchwifi-rollback-079\` |

Git：分支 `hdh001-winarm-bounded-write`
- `705901b` 0.7.2 checkpoint（tag `wifi-0.7.2-checkpoint`）
- `a2b4397` 0.7.3.0 + 0.7.4.0 全部改动（14 files, +874/−38）

## 三、0.7.3.0：按 GitHub 重构断连策略

移植内容（对照 `wifi-ref\Windows-for-Switch-Drivers-hdh001\drivers\SwitchWifi`）：

| 文件 | 改动 |
|---|---|
| `windows/report.h` | 新增 `SWIFI_LEAVE_PHASE`（None/Command/Event/Readback/Confirmed）、`LeavePhase`/`LeaveStatus` 字段、`SwifiReportLeave()`；顺手清掉了原型被复制 3 份的重复块 |
| `windows/report.c` | 报告串加 `leave=%u/%08X`；实现 `SwifiReportLeave()` |
| `windows/join.c` | 新增 `ConfirmLinkDown()`：到点先报 Readback，再 `LinkIsDown()` 回读 BSSID —— **确认已断 → `Down()` 正常收尾**（清密钥、报去关联、断媒体、试下一个候选，网卡保持可用）；**未确认/IO 错 → `Poison()`**；`JoinTimeout()` 里原来"2 秒到点无论结果都 `JoinReset` + `FirmwareDirty=TRUE` + `Poisoned=FALSE`"整段删除，改调 `ConfirmLinkDown`；`BeginLeave()` 补 Command/Event 两阶段上报 |

**保留**（这是 0.7.x 相对 Lite 的增量，与失败策略正交，实测有效）：
`Down()` 里 `Residual → FirmwareDirty`、`PrepareFirmwareForJoin()` 在下次 join 前做 `WLC_DOWN` 复位。
上机日志里每轮重连都出现 `dn=…/E0`，即这条复位路径**每轮都在正常工作**。

## 四、★ 0.7.4.0：真正卡住重连的缺陷（EAPOL 被当过期包丢弃）

### 4.1 症状链（0.7.3.0 上机实测）

```
13:49:09  netsh wlan disconnect            → device OK，adapter Disconnected，dn=1/D3
13:49:14  netsh wlan connect               → 成功下发
13:49:45  !! 30 秒仍未连上                 → device OK（没中毒！）但 adapter 一直 Disconnected
```
WLAN-AutoConfig 日志（`Microsoft-Windows-WLAN-AutoConfig/Operational`）逐次给出：

```
8000  无线网络关联开始 → 11000 关联完成        ← 关联是成功的
11006 无线安全失败：原因=动态密钥未在配置的时间范围内成功   ← 4 次握手超时
8002  失败原因：动态密钥未在配置的时间范围内成功
```
即：**关联 OK、握手不 OK**。而 `erx`（驱动看到的 EAPOL 帧数）在整个过程里**一直是 3**，
`kadd` 一直是 2 —— 一个 EAPOL 都没再收到，密钥一次都没再装。
唯一恢复手段是重启网卡（`Restart-NetAdapter` 之后 4 秒内就连上了）。

### 4.2 根因（主机侧复现，无需上机）

我们这棵树 0.7.2 相对 Lite 参考**删掉了两个回归测试**：
`tests\test_join_timeout.c`、`tests\test_receive_reconnect.c`。
把后者恢复并跑起来，第一处断言就红：

```
ASSERT FAILED: d->Rx[1].State==SwifiRxReady   test_receive_reconnect.c:174
```
它构造的正是现场：第一次关联成功 → 断开 → 重新 `SwifiDataBeginJoin`（epoch+1）→
EAPOL M1 落在**第一次关联期间就投递给固件**的缓冲区上 → 期望"接收成功"，我们丢弃。

对应代码（0.7.3.0 的 `windows/station_data.c:200`）：

```c
if ((!D->Associated || Slot->Epoch != D->Epoch) && !(Eapol && (Native || Direct))) {
    ++D->RxDrops; return;      /* ← 旧缓冲区上的 EAPOL 在这里被丢掉 */
}
```
`Slot->Epoch` 是**空缓冲区投递时**（`Service()` 的 RXPOST）写下的，代表"这块内存属于哪个关联"；
但 RXPOST 的缓冲区会在传输层存活很久，**它描述的从来不是"这个包属于哪个关联"**。
epoll 门 里只有 `Native || Direct` 的 EAPOL 才被豁免，`netsh` 重连场景下 AP 发来的
bridged/length-encapsulated EAPOL M1 不满足，于是被丢。

### 4.3 修法（0.7.4.0，与 GitHub 参考实现一致）

- `RxComplete()`：判据回到 `if (!D->Associated && !(Eapol && (Native || Direct)))`；
  **在到达时刻**写 `Slot->Epoch = D->Epoch; Slot->KeyEpoch = D->KeyEpoch;
  Slot->KeyInFlight = ...`，供同一函数后面的 `swifi_prekey_ethernet()` 判据使用。
- `Service()`（RXPOST）：**不再**写 `Epoch/KeyEpoch/KeyInFlight` —— 空缓冲区不对任何关联作声明。
- 队列侧的安全检查**原样保留**：`AdvanceRx()` 仍按 `Slot->Epoch`/`KeyEpoch`/`AcceptedKeyEpoch`
  拒绝"跨关联、跨密钥变更"的已排队包（测试里第 179–191 行专门守住这条）。

## 五、测试基线（这次补回来的东西）

`tools\Test-SwitchWifi.ps1` + `tools\HostTests\`：

- 构建主机**只有 WDK + LLVM，没有 MSVC CRT**（无 `INCLUDE/LIB`、无 `vcruntime.h`），
  普通 hosted 编译不可行；MSYS2 的 gcc 又因沙箱禁止命名管道跑不起来。
  → 12 个 core 测试 + 2 个生产代码测试改为 **freestanding** 编译：自带极小 shim
  （`memcpy/memmove/memset/memcmp/strlen`、`puts/printf/fprintf`、`malloc/calloc/free`、
  kernel32 入口）+ `chkstk.c`（x64 栈探测，缺了它 `test_control` 那个 90 KB 栈上 `model` 会 AV）。
- 两个生产代码测试沿用上游 `check-core.py` 的思路：**把 `join.c` / `station_data.c` 里的生产函数
  原样切出来**成 `.inc`，配桩函数编译运行（不是重写逻辑，测的就是驱动里的那段代码）。
- 结果：**14/14 通过**（含 `test_join_timeout`、`test_receive_reconnect`）。

```powershell
cd D:\paopao\WoA\Windows-for-Switch-Drivers
pwsh -File tools\Test-SwitchWifi.ps1                 # 全跑
pwsh -File tools\Test-SwitchWifi.ps1 -Filter 'ring'  # 只跑某个
```

## 六、上机验收（0.7.4.0）

`C:\SwitchWOA\wifi-lrtest2.log`（脚本 `wifi-lrtest2.ps1`，以 SYSTEM 计划任务运行，因为
断连会掐断 SSH 通道）：

| 轮次 | netsh connect | 重连耗时 | 速率 | 设备 | erx | kadd |
|---|---|---|---|---|---|---|
| 1 | 成功 | ~2 s | 702 Mbps | OK/— | 5 | 4 |
| 2 | 成功 | ~2 s | 866.5 Mbps | OK/— | 8 | 6 |
| 3 | 成功 | ~2 s | 866.5 Mbps | OK/— | 11 | 8 |
| 4 | 成功 | ~2 s | 866.5 Mbps | OK/— | 14 | 10 |
| 5 | 成功 | ~2 s | 866.5 Mbps | OK/— | 17 | 12 |

`kfail=0` 全程；`dn` 每轮 +2 = 一次 `0xD3`(主机断开) + 一次 `0xE0`(下次 join 前的固件复位)。

> `leave=0/00000000` 说明本轮**没有走到** `ConfirmLinkDown`：`netsh wlan disconnect` 是
> "主机请求断开"，走 `LeaveTask → 0xD3 → Down()` 这条路；`ConfirmLinkDown` 只在
> **中止/超时且固件没回终态事件**时才进。所以 0.7.3.0 那段策略由主机侧
> `test_join_timeout`（切的是生产函数）覆盖，而不是这条上机用例。

### 6.1 反复断连浸泡（上游明确"未验证"的那一项）

`wifi-soak.ps1`，连续 10 轮，脚本只做 disconnect → 等 2 s → connect → 轮询到 Up：

| 轮次 | 结果 | 重连耗时 | 速率 | 设备 | erx | kadd |
|---|---|---|---|---|---|---|
| 1 | OK | 58.1 s | 866.7 Mbps | OK/— | 33 | 16 |
| 2 | OK | 5.2 s | 6 Mbps→ | OK/— | 35 | 18 |
| 3 | OK | 7.2 s | 780 Mbps | OK/— | 38 | 20 |
| 4 | OK | 9.0 s | 866.5 Mbps | OK/— | 41 | 22 |
| 5 | OK | 7.4 s | 866.5 Mbps | OK/— | 44 | 24 |
| 6 | OK | 7.3 s | 866.5 Mbps | OK/— | 47 | 26 |
| 7 | OK | 6.6 s | 866.5 Mbps | OK/— | 50 | 28 |
| 8 | OK | 7.2 s | 866.5 Mbps | OK/— | 53 | 30 |
| 9 | OK | 7.2 s | 866.5 Mbps | OK/— | 56 | 32 |
| 10 | OK | 7.3 s | 866.5 Mbps | OK/— | 59 | 34 |

**10/10 重连成功，`kfail=0`，设备状态全程 OK，从未需要禁用/启用网卡。**
两点如实记账：

- 第 1 轮 58 s，其余 5–9 s —— 首轮慢，像是 WLAN 服务当时正在扫描（`netsh wlan connect`
  自身会阻塞到扫描结束），不是驱动报错。
- 第 2 轮重连瞬间只有 6 Mbps，随后几秒爬到 780–866.5 Mbps —— 速率自适应过程，正常。


## 七、上机安全网（这次的实操做法，建议保留）

断连测试会同时掐掉 SSH 传输，因此全部操作以 **SYSTEM 计划任务**下发、写日志、事后回读：

| 脚本（在 `C:\SwitchWOA\`） | 作用 |
|---|---|
| `install-07X.ps1` | 装驱动 + 回报版本/状态/链路 |
| `wifi-lrtest2.ps1` / `wifi-soak.ps1` | 断连重连验收 / 多轮浸泡 |
| `wifi-recover.ps1` | `Restart-NetAdapter` 兜底（13:40 那次真的救了场） |
| `wifi-rollback.ps1` | **守卫式回退**：没有 `wifi-07X-ok` 标记才装 0.7.9.0（=0.7.0.0 二进制）|

铁律补充：
- `pnputil` 只升不降，所以回退包必须"同一个二进制 + 更高 DriverVer + 重签 cat"
  （`pack-switchwifi-rollback.ps1`）。
- **`scp` 会挂但 `ssh` 能用**：本轮结尾 `scp` 反复报 `Connection timed out during banner exchange`，
  同时 `ssh` 执行命令完全正常（`Subsystem sftp C:\OpenSSH\sftp-server.exe`）。
  取日志改用 `ssh … "powershell -NoProfile -EncodedCommand <base64>"`，一次拉全（见
  `logs-20261118\device-logs-all.txt`）。
- 远端默认 shell 是 PowerShell，**复杂命令里的 `{}` 会在传输中被吃掉**
  （`MissingEndCurlyBrace`）；带循环/大括号的远程脚本一律走 `-EncodedCommand`。


## 八、还没解决的问题（明确挂账）

1. **`ConfirmLinkDown` 未被真实场景触发过**：它只在"中止/超时且固件没回终态事件"时进入，
   正常 `netsh disconnect` 走的是事件路径。要让它上机，需要造"固件丢事件"的场景
   （例如扫描/关联进行中强行断开、或 AP 侧突然消失）。主机侧测试已覆盖逻辑分支。
2. **`scan=` 反复出现 `C00000B5`（STATUS_IO_TIMEOUT）**：收尾时的状态快照是
   `scan=22/C00000B5`（22 次扫描、最后一次超时），中间也见过 `scan=7/00000000`。
   扫描超时没影响本轮断连/重连（20 次 join、34 次装密钥全部成功），但它是**目前最大的
   未解释现象**，下一步应该单独查（escan 中止/终态事件丢失？）。
3. 上游明说未验证、我们也仍未验的：持续吞吐、组密钥重协商（group rekey）、
   Erista 与 B01 在 PCIe 通道/UPHY 时钟/供电轨/PH0-PH1 GPIO 上的差异。
4. 0.7.2 里的 `Connect()` 仍保留 `J->Poisoned = FALSE` 的"自救"写法；由于 `Poison()` 一定会
   先 `SwifiPnpFailure()`（`WdfDeviceSetFailed(NoRestart)`），它救不回已失败的设备，
   属于历史遗留，未动。

## 八之补：收尾时的机器状态

```
驱动   switchwifi 0.7.4.0 (oem52.inf)   设备 ACPI\SWOA0D01\0 = OK / 无问题码
链路   WLAN Up 866.7 Mbps               计划任务：已全部清空（无 Wifi* 残留）
报告   erx=59 kadd=34 kfail=0 dn=32/E0 keys=18 add=34 del=32 kp=17 kg=17 leave=0/00000000
```


## 九、下一步建议

1. 造"固件丢事件"场景验证 `ConfirmLinkDown` 上机路径（最接近的办法：关联进行中 `netsh wlan disconnect`，
   或把 AP 断电 2–5 秒再恢复）。
2. 把 `leave=` 阶段做成可见指标，长期观察是否出现 `2/…`（事件丢失）或 `3/…`（回读不一致）。
3. 浸泡：跑 1–2 小时混合负载（DHCP 续租 + 大流量 + 周期性断连），盯 `kfail`、`RxDrops`、`dn`。
4. 视情况把 `FirmwareDirty`/`PrepareFirmwareForJoin` 这条 Erista 专门绕法汇报给上游分支。
