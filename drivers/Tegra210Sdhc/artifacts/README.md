# Tegra210Sdhc — prebuilt test-signed binaries

All three are the same driver (`nvtsdhc.sys`, ARM64, WDM/sdport miniport)
compiled from `../src` with **LLVM clang-cl + WDK 19041** and signed with the
project test certificate (`SwitchWOA Test Driver Signing`). They differ only in
build flags, so one file swap + one reboot switches between them.

| File | SHA-256 (first 8) | Card clock | DMA | Measured (fake Samsung EVO Plus 128 GB) |
|---|---|---|---|---|
| `nvtsdhc-sgdma-stable-57E79836.sys` | `57E79836` | CAR div 0 → ~102 MHz (**overclocked**) | SDPORT scatter/gather ADMA2-32 | **read 30–48 MB/s**, write ~10 MiB/s |
| `nvtsdhc-sgdma51-B506C640.sys` | `B506C640` | CAR div 1 → ~51 MHz (in spec) | SDPORT scatter/gather ADMA2-32 | not yet benchmarked (~24 MiB/s expected) |
| `nvtsdhc-blk128-pio-41EFF78A.sys` | `41EFF78A` | CAR div 0 → ~102 MHz | none (PIO, 128 blocks/call) | read 5.83 MiB/s, write ~5.2 MiB/s |

Build flags for each (see `../tools/build-stage.ps1`):

```
# stable / fastest (currently installed)
build-stage.ps1 -Profile sdport -NoAdvertise -FastPio -CarDivisor 0 -PollBlocks 128 -SgDma
# in-spec clock fallback
build-stage.ps1 -Profile sdport -NoAdvertise -FastPio -CarDivisor 1 -PollBlocks 128 -SgDma
# PIO-only fallback (no DMA at all)
build-stage.ps1 -Profile sdport -NoAdvertise -FastPio -CarDivisor 0 -PollBlocks 128
```

## Installing

The disk this driver serves is the boot volume, so the swap is a file replace
plus a reboot; the PnP binding (`Enum\ACPI\NVDA0212\0` → `Service=nvtsdhc`)
stays untouched. `../tools/deploy.ps1` does exactly that when run as SYSTEM
(it also keeps the previous file as `nvtsdhc.sys.old-<HHmmss>`).

```powershell
# on the device, as SYSTEM (e.g. via a scheduled task)
Copy-Item C:\Windows\System32\drivers\nvtsdhc.sys C:\Windows\System32\drivers\nvtsdhc.sys.old
Copy-Item .\nvtsdhc-sgdma-stable-57E79836.sys C:\Windows\System32\drivers\nvtsdhc.sys
shutdown /r /t 5 /f
```

If the result is an unbootable disk (`0x7B INACCESSIBLE_BOOT_DEVICE`), pull the
card and use `../tools/restore-good-driver.ps1 -Volume F:` on the development PC
(it captures the offline `Diag` evidence first, then restores a known-good
`nvtsdhc.sys` and force-unloads the mounted hive so the card can be ejected).

## What to read afterwards

`HKLM\SYSTEM\CurrentControlSet\Services\nvtsdhc\Parameters\Diag`:

- `BusSpeed` (2 = High Speed 3.3 V), `ClockKhzActual`, `BaseClockKhz`
- `SgDmaRequests / SgDmaBlocks / SgDmaErrors / SgDmaAdmaErr` — DMA traffic and faults
- `PioCalls / PioCallBlocks / PioPortUs / PioPollUs` — the PIO path
- `CommandErrors`, `ErrorInterrupts`, `LastErrors`, `LastErrorCommand`

Full background and measurements: `../SDR104-T210-交接简报.md` (sections 17–24).
