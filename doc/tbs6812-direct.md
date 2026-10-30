# TBS6812.dll: warm ISDB-S3 direct control

This extension replaces per-channel vendor BDA tuning with raw I2C through
property 0x19. It supports **TBS6812 Tuner 0 / Capture 0, ISDB-S3 only**.
The vendor Windows driver and the BonDriver_BDA capture graph remain in use.
The existing dantto4k DLL remains responsible for conditional access and TS remux.

## Lifecycle

1. Validate the PCI model, tuner number, raw-I2C property and satellite mode.
   An incompatible card/tuner is rejected, not silently used through another path.
2. The first `LockChannel` returns `E_NOINTERFACE` so BonDriver_BDA prepares the
   board with its normal vendor tune. Only this initial tune takes the slow path.
3. `PostLockChannel` checks lock, chip ID 0x091, the 24 MHz clock and active S3/TLV
   mode before adopting the chip. It saves the initial IF, stream ID and output
   settings. A failed preparation requires close/reopen.
4. Later tunes pause TLV output, set the stream ID and Freia RF frequency, request
   soft reacquisition, enable parallel data pins and restart TLV output. Lock is
   polled every 5ms within `LockTimeoutMs` (default 3000ms, bounded 100..10000ms).
   A failed direct tune is reported as failure; it never resubmits a vendor tune.
5. On close, restore the first vendor-tuned carrier and original output settings.
   The chip is **not cold-reset** and the driver does not need a mode-cycle to
   recover. The initial preparation enables the first tuner's 13V supply; the
   extension leaves it enabled when handing the tuner back.

No card-wide mode changes, cold initialization, shared PLL reprogramming,
firmware loads or adjacent-tuner configuration are performed. A session-local
named kernel object prevents two extension instances from claiming Tuner 0.
The dedicated vendor supply property 0x15 is used for initial 13V LNB power.
The plugin serializes register access, including monitoring, with a mutex.

Right/left Japanese BS carriers are distinguished by their downconverted IF:
10,678 MHz LO for right and 9,505 MHz LO for left. The supported IF range is
950..3224 MHz. Frequencies above 2150 MHz use the Freia high-frequency path.
Requests to enable continuous tone or use a DiSEqC switch are rejected in this first version.
ISDB-S, terrestrial reception and Tuner 1 are not supported by this extension.

## Build and unit checks

```powershell
./tools/build-tbs.ps1
cmd /c tools\test-tbs6812.cmd
cmd /c tools\build-probe.cmd
```

The bus-level tests cover invalid input before writes, 8K frequency bytes and RF
path selection, restoring the bootstrap carrier, an injected partial I2C failure,
unfreezing the monitor after a failed read, and rejecting the wrong chip ID.

The main BonDriver now honors failures from pre/post lock hooks (while preserving
`E_NOINTERFACE` as unsupported). This prevents a rejected bootstrap adoption from
being reported as successful just because the vendor tune itself succeeded.

## Deploy without overwriting the baseline

```powershell
./tools/deploy-tbs.ps1 -TVTestDirectory 'C:\path\to\TVTest' -Direct
```

This deploys `BonDriver_BDA_4k.dll`, `TBS6812.dll`, the direct-control INI and
license notices to `4k8kdrver-direct`. Only `bondriverPath` in the outer
`BonDriver_dantto4k.ini` changes. The baseline `4k8kdrver-built` stays intact.
Configuration backups and verified SHA256 hashes are recorded under `work/`.
To restore the baseline, close TVTest and point `bondriverPath` back to
`.\4k8kdrver-built\BonDriver_BDA_4k.dll`, or restore the recorded INI backup.

`TBS6812.log` beside the DLL distinguishes bootstrap, direct tune, timeout/I2C
failures and restoration. It logs frequencies, stream IDs and errors, not card
IDs or conditional-access data.

## Hardware checks

The probe accepts comma-separated channel names and keeps one tuner open:

```powershell
./work/bon-driver-probe.exe 'C:\path\to\4k8kdrver-direct\BonDriver_BDA_4k.dll' 'BS07/TS0,BS14/TS0,BS07/TS0,BS11/TS0' 6 ./work/capture.mmts
```

It writes separate numbered captures. Validate each capture's actual TLV stream
ID/service and MMTP continuity, then repeat through dantto4k and inspect the
converted TS and HEVC streams. `SetChannel` timing includes BonDriver_BDA's own
post-lock delay, while the extension log reports direct acquisition time.
Reopen the unchanged baseline after closing direct mode to verify hand-back.
File/API verification does not certify TVTest live display, long recordings or
simultaneous use of both tuners.

The ported satellite sequence and C/N curve are GPL-2.0-or-later. See
`BdaSpecials/TBS6812/NOTICE.md` and `COPYING` for origin and license details.
