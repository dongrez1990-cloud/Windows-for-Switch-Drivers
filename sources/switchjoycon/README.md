# switchjoycon 鈥?鍒濅唬鏈猴紙Erista锛塉oy-Con 瀵艰建鎵嬫焺椹卞姩锛圵indows 11 ARM64锛?
> 2026-11-16 璧枫€傛帴銆孲witch椹卞姩WiFi涓庤摑鐗欒繛鎺ラ棶棰樻帓鏌ャ€嶄細璇濈户缁細Wi-Fi / 钃濈墮 / 椋庢墖 / 瑙﹀睆 / 闊抽閮藉凡鎵撻€氾紝
> **鎵嬫焺鏄渶鍚庝竴鍧?*锛坄璇婃柇-WinX鍒濅唬鏈洪€傞厤.md` 搂鍥涗箣浜岋細涓婃父 14 涓┍鍔ㄧ洰褰曢噷娌℃湁浠讳綍鎵嬫焺椹卞姩锛夈€?
---

## 涓€銆佺粨璁哄厛璇达細鍒濅唬鏈烘墜鏌勭殑涓ゆ潯璺?
| 璺緞 | 纭欢 | 鐜扮姸 | 闅惧害 |
|---|---|---|---|
| **瀵艰建 Joy-Con**锛堟帉鏈烘ā寮忥紝鎻掑湪鏈鸿韩涓婏級 | 宸﹀彸鍚勪竴璺?UART锛?*鍙嶇浉**锛? Mbaud 璧锋墜 鈫?3 Mbaud | 鏃犻┍鍔?| 涓紙鏈枃妗ｉ噸鐐癸級 |
| 鏃犵嚎 Pro 鎵嬫焺 / 鎷嗕笅鐨?Joy-Con | 钃濈墮 BCM4356A3 | `switchbt` 1.0.0.19 宸查€氾紝浣?HID 閰嶅杩樻湁闂 | 寰?BT 渚ф敹灏?|

鏈粨搴撳仛**绗竴鏉?*锛氬杞?Joy-Con 鈫?Windows 鐨?HID gamepad锛圴HF锛夈€?
## 浜屻€佺‖浠跺彇璇侊紙鏉ヨ嚜 switchroot Erista 璁惧鏍?+ 鏈満鍥轰欢锛?
`WoA\ref\dts\kernel-dts\nx-platforms\tegra210-odin-common.dtsi`锛?
```dts
/* Right Joycon */
serial@70006040 {                       /* UART-B */
    status = "okay";
    nvidia,invert-txd; nvidia,invert-rts;
    joyconr { charger-supply = <&en_vdd_jcr_chgr>;
              detect-en-gpios = <&gpio TEGRA_GPIO(G, 0) GPIO_ACTIVE_HIGH>;  /* 灏辨槸 UART-B TX 鐒婄洏 PG0 */
              detect-gpios    = <&gpio TEGRA_GPIO(H, 6) GPIO_ACTIVE_LOW>;   /* 鎻掑叆妫€娴?H6 */
    };
};
/* Left Joycon */
serial@70006200 {                       /* UART-C */
    joyconl { charger-supply = <&en_vdd_jcl_chgr>;
              detect-en-gpios = <&gpio TEGRA_GPIO(D, 1) GPIO_ACTIVE_HIGH>;  /* UART-C TX 鐒婄洏 PD1 */
              detect-gpios    = <&gpio TEGRA_GPIO(E, 6) GPIO_ACTIVE_LOW>;   /* E6 */
    };
};
```

| 椤?| 鍙虫墜鏌?| 宸︽墜鏌?|
|---|---|---|
| UART | UART-B @ `0x70006040` | UART-C @ `0x70006200` |
| 涓柇锛圙IC SPI锛?| 69 | 70 |
| 寮曡剼锛坧inmux 鍋忕Щ锛?| TX PG0 `0x0F4` / RX PG1 `0x0F8` / RTS PG2 `0x0FC` / CTS PG3 `0x100` | TX PD1 `0x104` / RX PD2 `0x108` / RTS PD3 `0x10C` / CTS PD4 `0x110` |
| 鎻掑叆妫€娴?GPIO | PH6 = 62锛堜綆鏈夋晥锛?| PE6 = 38锛堜綆鏈夋晥锛?|
| 鍏呯數浣胯兘 GPIO | PK3 = 83 | PCC3 = 227锛坄od tegra210-odin-gpio.dtsi` 閲?suspend 鏃舵媺浣庯級 |
| 琛屽弽鐩?| invert TXD + invert RTS | 鍚屼笂 |
| 鏃堕挓锛堝浐浠跺父閲忓凡瀛樺湪锛?| `CLK_RST_CONTROLLER_CLK_SOURCE_UARTB = 0x17C`锛宍CLK_L_UARTB = 1<<7` | `CLK_RST_CONTROLLER_CLK_SOURCE_UARTC = 0x1A0`锛宍CLK_H_UARTC = 1<<23` |

琛ュ厖浜嬪疄锛?
- DSDT 閲?*宸茬粡鏈変竴涓?* `UAR2`锛堝彸鎵嬫焺 UART锛宍NVDA0100`锛宍CRS3(0x70006040,0x40,0x45,0x09,0x02)`锛夆€斺€旀槸涓婃父褰撳勾鐣欎笅鐨勶紝娌℃湁瀵瑰簲椹卞姩锛學indows 渚т笉璁わ紱鎴戜滑浼氭敼鎴愯嚜鏈?HID 骞惰ˉ宸︽墜鏌勩€?- 钃濈墮 UART-D 鐨?3 Mbaud 璧版硶宸查獙璇佸彲鐢紙`switchbt.c: CAR_UART_3M = (1<<24)|15 鈫?48 MHz = 16脳3 Mbaud`锛夛紝瀵艰建鐨?3 Mbaud 鍚屾硶锛?*1 Mbaud** 鐢?`(1<<24)|49 鈫?816/51 = 16 MHz = 16脳1 Mbaud`锛?08 MHz PLLP 涓嶈兘琚?16 MHz 鏁撮櫎锛屽繀椤昏蛋 bit24 瑕嗙洊锛夈€?- 鍙嶇浉锛歴witchroot `serial-tegra.c` 鎶?`nvidia,invert-txd/rts` 鍐欐垚 **UART 鍋忕Щ 0x08锛坄TEGRA_UART_IRDA_CSR`锛?* 鐨?bit1/bit3锛涗絾 0x08 鍚屾椂涔熸槸 FCR锛坄switchbt` 鍐?0x87 寮€ FIFO锛夈€?*杩欐槸蹇呴』涓婃満瀹炴祴鐨勭涓€涓偣**锛堝啓 0x0A 鏄惁鐮村潖 FIFO銆佹彙鎵嬫槸鍚﹀洜姝ら€氾級銆?
## 涓夈€佸崗璁弬鑰冿紙宸茶惤鍦板埌 `ref/`锛?
| 鏂囦欢 | 璇存槑 |
|---|---|
| `ref/joycon-serdev.c` | switchroot L4T 5.1.2 鐨勫杞?Joy-Con 椹卞姩锛?13 KB锛?*鍗忚鐪熸簮**锛?|
| `ref/serial-tegra.c` | Tegra UART 琛屽弽鐩?娉㈢壒鐜囧啓娉?|

閫愬抚鍗忚瑙?**`ref/PROTOCOL.md`**锛堟湰浠撳簱浠庝笂闈㈡簮鐮佹彁鐐硷紝鍙洿鎺ョ収鎶勬垚 Windows 浠ｇ爜锛夈€?
## 鍥涖€佸垎闃舵璁″垝

| 闃舵 | 鍐呭 | 浜х墿 | 楠屾敹 |
|---|---|---|---|
| M0 | 鍗忚/纭欢鍙栬瘉钀藉湴 | 鏈?README銆乣ref/PROTOCOL.md`銆乣ref/*.c` | 鏂囨。鍙嚜娲藉鐜?|
| M1 | 鍥轰欢锛歱inmux + 鏃堕挓 + ACPI锛坄UAR2`/`UAR3` 鈫?`SWOA0E01`/`SWOA0E02`锛屽姞 CAR 绐楀彛涓庢娴?GPIO 璧勬簮锛?| 鏀?`NintendoSwitchPkg`锛岄噸缂?bl33 | 寮€鏈哄悗璁惧绠＄悊鍣ㄥ嚭鐜颁袱涓€屾湭鐭ヨ澶囥€嶏紝璧勬簮 0x70006040/0x70006200 + IRQ 69/70 |
| M2 | 椹卞姩锛歎ART 16550 + 鎻℃墜锛? Mbaud 鈫?3 Mbaud锛? 璇诲彇 MAC | `switchjoycon.sys` | 涓插彛鑳界湅鍒?`19 81 03` 鍖呫€丮AC 姝ｇ‘ |
| M3 | HID锛歏HF gamepad锛堟寜閿?+ 鎽囨潌 + 甯藉瓙锛夛紝宸?鍙冲垎璁惧 | 鍚屼笂 | `joy.cpl` / XInput 娴嬭瘯宸ュ叿鑳借鍒?|
| M4 | 鐑彃鎷旓紙妫€娴?GPIO + 鎺夌嚎閲嶈繛锛夈€佸厖鐢典娇鑳姐€侀渿鍔?LED锛堝彲閫夛級 | 鍚屼笂 | 鎷斿嚭/鎻掑洖鑷姩閲嶆柊鎻℃墜 |
| M5 | 涓婃満楠屾敹 + 瀹夎/鍥炴粴鑴氭湰 | `tools/install.ps1` | 鎺屾満妯″紡涓嬭兘鐜?|

## 浜斻€佸綋鍓嶇姸鎬侊紙2026-11-16 澶滐級

宸茶惤鍦帮紙鏈粨搴擄級锛?
| 浜х墿 | 浣嶇疆 | 鐘舵€?|
|---|---|---|
| 椹卞姩楠ㄦ灦锛圲ART + 鎻℃墜 + 杈撳叆鎶?+ VHF HID + 鎻掑叆妫€娴?+ 鐑彃鎷旓級 | `src/switchjoycon.c`銆乣src/jc.h` | **缂栬瘧骞舵祴璇曠鍚嶉€氳繃**锛坄out/switchjoycon.sys` 27 KB锛宍switchjoycon.cat`锛?|
| INF锛坄ACPI\SWOA0E01` / `ACPI\SWOA0E02`锛孒IDClass + vhf 涓嬭繃婊ら┍鍔級 | `switchjoycon.inf` | 瀹屾垚 |
| 鍥轰欢 ACPI锛歚UAR2/UAR3` 鈫?涓や釜 SWOA 璁惧锛屽惈 `JINI(1)` AML锛坧ad mux + 16 MHz 鏃堕挓 + 鍏呯數浣胯兘锛?| `WoA\build\NintendoSwitchPkg\AcpiTables\Dsdt\Dsdt.asl` | **v44 宸茬紪璇?*锛歚WoA\rebuild\out\bl33-swoa-v44.bin`锛坰ha256 `9684d600鈥?302`锛夛紝DSDT 閲?`SWOA0E01/SWOA0E02/JINI` 鍧囧凡纭瀛樺湪 |
| 鍙樹綋鍏ュ彛 | `WoA\build\build-variants.sh` 鐨?`v44` | `bash build-variants.sh v44` |
| 瀹夎/鎺㈤拡鑴氭湰 | `tools/install.ps1`锛圫SH锛氳椹卞姩 / 璇?`JcState` 绛?/ 閲嶅惎璁惧锛?| 瀹屾垚 |
| 涓婂崱鑴氭湰 | `tools/deploy-card.ps1`锛堝啓 `switchroot\joycon\` + hekate 鑿滃崟 `44-joycon`锛?| 瀹屾垚 |
| 鏋勫缓鑴氭湰 | `tools/build.ps1` | 瀹屾垚 |

鐜锛歋witch 鐜板湪**鍦ㄧ潯**锛坄ssh 100.106.117.117` TCP 閫氥€乥anner 瓒呮椂锛滿odern Standby锛夛紱鍗″湪鏈哄櫒閲岋紝PC 鍙湁涓€涓?C:/D:銆?PC 娌℃湁澶栫綉锛圕lash GUI 娌¤窇锛岀郴缁熶唬鐞?127.0.0.1:52029 宸插け鏁堬級锛屼絾 `fastly.jsdelivr.net` 鐨?IPv6 鍙敤锛屽弬鑰冩簮鐮佸氨鏄粠閭ｉ噷鎷夌殑銆?
## 鍏€佷笂鏈洪『搴忥紙Switch 閱掔潃鏃讹級

1. 鍗℃彃杩?PC 璇诲崱鍣?鈫?`pwsh -File tools\deploy-card.ps1`锛堣嚜鍔ㄦ壘 ESP锛屽啓 `switchroot\joycon\bl33.bin` + `bootloader\ini\44-joycon.ini`锛夛紱
2. 鍗″洖 Switch 鈫?hekate 閫?**`Windows 11 + JoyCon v44`**锛?3. 杩涚郴缁熷悗锛堝睆涓婂厛鐪?`R3xx` 涓?+ 鏂扮殑 `[bds]` 琛岋紱DSDT 鍙樺寲涓嶄細鎵撳嵃锛屽睘姝ｅ父锛夛細
   ```
   pwsh -File tools\install.ps1 -Install      # scp + pnputil 瀹夎锛岃姹?Switch 閱掔潃
   pwsh -File tools\install.ps1 -Probe        # 璁惧鐘舵€?+ JcState/JcReports/JcErrors/JcTimeouts/JcAcpiInit
   ```
4. 鍒よ锛?   - `JcAcpiInit=1` 鈫?AML 璧锋潵浜嗭紱`=0` 鈫?JINI 澶辫触锛堝厛鏌?DSDT 鏈夋病鏈夎 _OSI 涔嬬被鎸℃帀锛夛紱
   - `JcState`锛?=鏈彃銆?=妫€娴嬬ǔ瀹氭湡銆?=榄旀暟銆?=鎻℃墜銆?=璇籑AC銆?=HID鏂紑銆?=HID杩炴帴銆?=鍛ㄦ湡銆?=鎶ュ憡妯″紡銆?=Ready锛?   - `JcReports` 澧為暱 鈫?鎵嬫焺閫氫簡锛沗JcTimeouts` 涓€鐩存定 鈫?鎻℃墜鏃犲洖鍖咃紙鍏堟€€鐤?搂8 鐨?0x08 鍙嶇浉鎴栨椂閽燂級銆?5. 鑻ヨ**涓嶈椹卞姩**鍏堥獙璇佺‖浠讹細鎶?`JcState` 鍏ㄦ祦绋嬭蛋涓嶉€氭椂锛岀洿鎺ョ湅 `JcErrors`/`JcTimeouts` 鐨勬瘮鍊兼潵鍒ゆ柟鍚戯紝涓嶇敤涓?Windbg銆?
## 涓冦€佷笅涓€姝ワ紙鎸夐『搴忥級

1. **涓婃満绗竴娴?*锛歷44 + 椹卞姩锛岃 `JcState`/`JcReports`锛涗笉閫氬氨鎸?搂8 閫愰」璇曪紙0x08 鍙嶇浉鍊笺€丆AR 鍒嗛銆乸ad pull/tristate锛夈€?2. 閫氫簡浠ュ悗锛氭妸 1 Mbaud 鈫?**3 Mbaud + 2 鍋滄浣?*锛坄PROTOCOL.md` 搂3 绗?4 姝ワ紝`JcSendSubcmd` 涔嬪悗鎸?switchroot 鐨?`joycon_change_baud`锛夛紝骞舵妸 CAR 鍒囨崲鍒?`(1<<24)|15`銆?3. 鎽囨潌鏍″噯锛圫PI 瀛愬懡浠?0x10锛屽湴鍧€ 0x603D/0x6046锛夋浛鎹㈤粯璁?1024/2048/3072銆?4. 闇囧姩 / 鐜╁鐏紙鍙€夛紝`0x30` / `0x48` 瀛愬懡浠わ級銆?5. 鏃犵嚎 Pro 鎵嬫焺璧?`switchbt` 閭ｆ潯璺椂鍐嶅洖鍒?BT 渚ч厤瀵归棶棰樸€?

### 2026-11-16 22:2x 已上卡（从 Switch 内部写 ESP，没拔卡）

| 项 | 值 |
|---|---|
| ESP 挂载 | `Set-Partition -DiskNumber 0 -PartitionNumber 1 -NewDriveLetter V`（health=Full Repair Needed，写前已确认可写、写完 sha 校验一致，最后 `Remove-PartitionAccessPath` 卸掉） |
| 固件 | `V:\switchroot\winx-v44\bl33.bin`（sha256 `9684D600E519D3BD560504DB51C95861CF767AC2ABB059B839347CD017651302`）+ 从 `winx-v42` 复制的 `bl31.bin` |
| 菜单 | `V:\bootloader\ini\48-swoa-v44.ini` → **`[SWOA v44 + JoyCon rail UART-B/UART-C  <== try 1st]`**，`id=WINXV44`，`cpu_oc=1785600` |
| 驱动 | 已装进 Switch 驱动库：`oem18.inf`（`pnputil /add-driver … /install /force` 成功），文件在 `C:\SwitchWOA\drivers\switchjoycon\` |
| 回滚 | hekate 里选 `00-swoa-v42`（原样保留 `winx-v42`，sha `BD3DF9CF…`） |

开机后设备会出现 `ACPI\SWOA0E01\1` / `ACPI\SWOA0E02\2`，驱动按 INF 自动绑定；读状态：

```
pwsh -File tools\install.ps1 -Probe
```

`JcState` 定义见上一节；`JcAcpiInit=1` 表示 AML 起来了。

### 2026-11-16 深夜：v46 上机第一轮诊断（重要）

- 驱动日志见 `tools/probe-rail.ps1`（SSH 读 `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SWOA0E0*\Device Parameters`）。
- **v45 的 DSDT 中断号写错了**：UART-B 是 GIC SPI 37 → GSIV **69**，UART-C 是 SPI 46 → GSIV **78**
  （依据 `WoA\ref\k_tegra210.dtsi`）；v46 已改。曾经的 101/102 让 ISR 永远不触发。
- v46 + 驱动 0.1.4.0 的实测：
  `JcAcpiInit=1`（JINI 成功）、`JcScratch=1`（UART 时钟/复位已就绪）、`JcIrqRaw=69/78`（ACPI 资源已正确）、
  `JcIerProbe=0x15`（写进去能读回）、但 `JcIir=0xC1`（IIR 仍报无中断）、`JcReports=0`、`JcState=3`（卡在握手）。
- **关键证据**：`JcRxTail = 7d 00 00 64 19 81 03 07 00 a5 02 02 7d 00 00 64`
  → 总线上确实有 `19 81 03`（RX 方向的完整帧，命令 0xA5＝握手应答帧头），
  但帧内头字段与发出去的 `02 01 7E` 相比出现了 `^0x03` 的位错（`01→02`、`7E→7D`），且前后夹着 `7d 00 00 64`。
  下一步要判定：这是 (a) Joy-Con 的真实应答但波特率/采样有偏差，(b) 反相只做了一半，还是 (c) 导轨是单线双向、我们听到的是自己的回波。
- 中断为什么不来还没定论（IER 能写能读、IIR 却恒 `0xC1`，`JcLsr` 高字节 0x200 也待查），
  所以 v46 之后 RX 仍靠定时器轮询，丢字节严重。

下一轮实验（要改驱动，不用动固件）：
1. RX 尾巴扩到 64 字节，并加"停发 5 秒"开关（注册表 `JcSilent=1`）——静默时若仍有字节，是 Joy-Con 自说自话/噪声；若静默即停，则我们听到的是自己的 TX 回波；
2. 波特率扫：DLL/DLM = 1（16 MHz 精确）/ 25（PLLP 直分，1.02 Mbaud）/ 2，看哪一种能让 `01 7E` 干净地回来；
3. 反相位试 `0x00 / 0x02 / 0x0A`（IRDA CSR 0x20）三档，看 RX 帧命令字节是否变干净。

### v46 后续两轮（驱动 0.1.5–0.1.7）

- `JcRxTail`（64 字节）反复出现同一条完整帧：`19 81 03 07 00 a5 02 02 7d 00 00 64`
  → **Joy-Con 确实在应答握手**（RX 方向 magic `19 81 03`、命令 `0xA5`），
  说明 pad mux / 16 MHz 时钟 / 反相这三样对短帧是通的。
- 修掉了我自己的一个匹配 bug：`0xA5` 应答应该只比命令字节（switchroot 也是这么做的），
  我之前还要求 `header_data[0]==0xA5`，所以永远匹配不上。0.1.5.0 已修。
- 现在剩下的拦路虎：**帧解析器一次都没凑出整帧**（`JcFrames=0`、`JcAck=0`），
  而同样的字节流在轮询缓冲里是干净的。0.1.7.0 把 `JcFeed()` 直接接到轮询读取处（绕过环形缓冲）。
- 注意：`pnputil /restart-device` 之后设备仍绑在旧包（`JcVer=106`），
  需要用 `pnputil /delete-driver oem24.inf /uninstall /force` 或重启一次才会换到新包。
- UART 中断仍未触发（`JcIir=0xC1` 恒定、`JcLsr` 高字节 0x200），
  优先级排在"先把握手走通"之后：握手能过，再决定是修中断还是把轮询周期压到 1 ms + FIFO 触发。
