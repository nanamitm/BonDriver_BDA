# TBS6812.dll: warm ISDB-S / ISDB-S3 direct control

This extension replaces per-channel vendor BDA tuning with raw I2C through
property 0x19. It supports **TBS6812 Tuner 0/1 with matching Capture 0/1,
ISDB-S and ISDB-S3**, including two simultaneous receiver processes.
The shared-driver configuration can automatically claim an available slot.
The vendor Windows driver and the BonDriver_BDA capture graph remain in use.
The existing dantto4k DLL remains responsible for S3 conditional access and TS remux.
Conventional ISDB-S emits 188-byte TS directly; see [ISDB-S setup](tbs6812-isdb-s.md).

## Lifecycle

1. Validate the PCI model, tuner number, raw-I2C property and satellite mode.
   An incompatible card/tuner is rejected, not silently used through another path.
2. The first `LockChannel` returns `E_NOINTERFACE` so BonDriver_BDA prepares the
   board with its normal vendor tune. Only this initial tune takes the slow path.
3. `PostLockChannel` checks lock, chip ID 0x091, the 24 MHz clock and the configured S/TS or S3/TLV
   mode before adopting the chip. It saves the initial IF, stream ID and output
   settings. A failed preparation requires close/reopen.
4. Later tunes pause TS/TLV output, set the stream ID and Freia RF frequency, request
   soft reacquisition, enable parallel data pins and restart the configured output. Lock is
   polled every 5ms within `LockTimeoutMs` (default 3000ms, bounded 100..10000ms).
   A failed direct tune is reported as failure; it never resubmits a vendor tune.
5. On close, restore the first vendor-tuned carrier and original output settings.
   The chip is **not cold-reset** and the driver does not need a mode-cycle to
   recover. The initial preparation enables the first tuner's 13V supply; the
   extension leaves it enabled when handing the tuner back.

No card-wide mode changes, cold initialization, shared PLL reprogramming,
firmware loads or adjacent-tuner configuration are performed. A session-local
named kernel object per device path and slot prevents duplicate claims.
The dedicated vendor supply property 0x15 is used for initial 13V LNB power on
Tuner 0 only; Tuner 1 does not change that shared supply. Both antenna inputs
must have an appropriate satellite feed (and externally powered LNB where needed).
Each slot uses its own input-pin property interface and Freia address: 0x60 for
Tuner 0, 0x61 for Tuner 1. The demodulator addresses are 0x64/0x66 on each bus.
The vendor-established reference clock/output remains unchanged on both slots.
The plugin serializes register access, including monitoring, with a mutex.

Right/left Japanese BS carriers are distinguished by their downconverted IF:
10,678 MHz LO for right and 9,505 MHz LO for left. The supported IF range is
950..3224 MHz. Frequencies above 2150 MHz use the Freia high-frequency path.
Requests to enable continuous tone or use a DiSEqC switch are rejected in this first version.
ISDB-S uses 950..2150 MHz IF and its own RF filters, TS output, TSID and monitors.
Terrestrial reception is not supported by this extension.

## Build and unit checks

```powershell
./tools/build-tbs.ps1
cmd /c tools\test-tbs6812.cmd
cmd /c tools\build-probe.cmd
```

The bus-level tests cover invalid input before writes, 8K frequency bytes and RF
path selection, restoring the bootstrap carrier, an injected partial I2C failure,
unfreezing the monitor after a failed read, and rejecting the wrong chip ID.
They also check independent RF addresses and reject tuner indexes outside 0/1.

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

### Two receivers

#### One shared BonDriver with automatic allocation

```powershell
./tools/deploy-tbs.ps1 -TVTestDirectory 'C:\path\to\TVTest' -Direct -AutoSelect
```

Both TVTest processes select the same `BonDriver_dantto4k.dll`. The auto template
matches the model without specifying a slot:

```ini
[Tuner]
UseSpecial="TBS6812"
FriendlyName0="TBS 6812 ISDB-T/S/S3 Tuner"
CaptureFriendlyName0="TBS 6812 ISDB-T/S/S3 Capture"
```

BonDriver_BDA's existing per-device named semaphore atomically claims a tuner
across processes. A busy candidate is skipped; if all candidates are busy,
OpenTuner fails. Windows supplies enumeration order, so slot 0 is not guaranteed
to be chosen first. Once opened, the slot remains assigned until CloseTuner;
changing channels does not allocate another tuner. TBS6812's CheckCapture rejects
a different slot or PCI board before graph capture starts.

The deployment disables the shared optional MMTS dump and archives the former
`BonDriver_dantto4k_Tuner1.dll`, INI and ch2 under `work/deployment-backups`.
The rebuilt dantto4k outer DLL from the previous dual-receiver setup is reused.

#### Explicit assignment takes precedence

An INI that specifies one numbered tuner only considers that tuner. For example:

```ini
[Tuner]
UseSpecial="TBS6812"
FriendlyName0="TBS 6812 ISDB-T/S/S3 Tuner 1"
CaptureFriendlyName0="TBS 6812 ISDB-T/S/S3 Capture 1"
```

If Tuner 1 is busy, this configuration fails even when Tuner 0 is free. Existing
fixed INIs retain this behavior. `-Direct -TunerIndex 0` or `-Direct -TunerIndex 1`
deploys a fixed configuration; these options cannot be combined with -AutoSelect.
If multiple numbered FriendlyName entries are deliberately listed, only those
listed candidates are considered, in their configured order.

#### Separate explicitly assigned receivers (alternative)

```powershell
./tools/deploy-tbs.ps1 -TVTestDirectory 'C:\path\to\TVTest' -Direct -Dual
```

The original `BonDriver_dantto4k.dll` stays pinned to Tuner 0. A second outer
`BonDriver_dantto4k_Tuner1.dll` and its INI/channel file select the matching
Tuner 1/Capture 1 via `4k8kdrver-direct-tuner1`. The outer DLL is copied unchanged;
the new receiver uses the same existing decoder/card settings, with its optional
MMTS dump disabled to prevent two processes writing the same dump file.
Start two TVTest processes and select a different outer BonDriver in each.
Each receiver still needs one vendor bootstrap per open, followed by direct tunes.
These are explicitly assigned slots, without automatic idle-tuner selection.

The original October 21 dantto4k binary was observed hanging in
`WinSCard!SCardDisconnect` from static destruction during `ExitProcess`, after
both tuner and DLL-release logs had completed. For reliable process exit, use
the dantto4k companion fix that skips PC/SC calls only during Windows process
termination (`DLL_PROCESS_DETACH` with non-null reserved argument). Ordinary
destruction still releases the card/context. Deploy a verified rebuilt outer DLL
to both wrappers with `-DanttoWrapperPath 'C:\path\to\BonDriver_dantto4k.dll'`.

For the raw simultaneous test, stage the inner DLL, extension and matching INI
under `work/dual-stage/tuner0` and `tuner1`, then run:

```powershell
./tools/test-dual-tbs6812.ps1
```

The test keeps each slot streaming while the other bootstraps, switches between
4K/8K and different stream IDs, closes and reopens. Captures and per-second byte
counts/data gaps are saved in `work/dual-test`. Inspect TLV/MMTP continuity and
actual stream IDs in addition to successful API return values.

## Hardware checks

Outstanding ISDB-S3 validation and improvements are tracked in
[tbs6812-isdb-s3-todo.md](tbs6812-isdb-s3-todo.md). The user has confirmed
two-window TVTest live display and viewing with the shared auto-allocation build.

The probe accepts comma-separated channel names and keeps one tuner open:

```powershell
./work/bon-driver-probe.exe 'C:\path\to\4k8kdrver-direct\BonDriver_BDA_4k.dll' 'BS07/TS0,BS14/TS0,BS07/TS0,BS11/TS0' 6 ./work/capture.mmts
```

It writes separate numbered captures. Validate each capture's actual TLV stream
ID/service and MMTP continuity, then repeat through dantto4k and inspect the
converted TS and HEVC streams. `SetChannel` timing includes BonDriver_BDA's own
post-lock delay, while the extension log reports direct acquisition time.
Reopen the unchanged baseline after closing direct mode to verify hand-back.
File/API verification does not certify TVTest live display or long recordings.

### Verified on October 30, 2026

Both slots received simultaneously. In both directions, one slot streamed for
55 seconds while its peer bootstrapped, selected BS11/TS0, BS14/TS0 (8K),
BS07/TS1 and BS07/TS0, closed and reopened. The held carriers had zero TLV/MMTP
sync losses/discontinuities and zero converted-TS sync/continuity/transport errors.
Both rebuilt-dantto4k receiver sequences exited normally with the companion
PC/SC process-exit fix. The dantto4k build and all 11 regression suites passed.

This does not promise a clean boundary at SetChannel: a short old-stream tail
and continuity jumps were observed on some switched captures, including an old
PAT at the beginning of one converted TS. After excluding approximately the
first 2 MiB, that capture identified the requested B070/service 151 with zero
continuity errors. The continuously receiving peer was unaffected. Initial
vendor preparation still takes about nine seconds; subsequent raw SetChannel
calls took roughly 0.7–1.1 seconds in the standalone Tuner 1 check.

Automatic allocation was then verified with two processes loading exactly the
same DLL and INI. A third OpenTuner failed while both slots were busy, and a
new process reused the first slot after it closed while the peer kept receiving.
Explicitly pinning either busy slot failed even when the other was free; both
fixed configurations also opened successfully once free. The raw TLV/MMTP and
converted TS captures had zero sync/continuity errors, with the requested B070,
B0B0 and B0E0 stream IDs. The final installed shared outer DLL was also opened
by two processes concurrently, selecting different slots and exiting normally.
`tools/test-auto-tbs6812.ps1` runs the allocation/rejection/reuse/fixed tests
against staged auto/fixed0/fixed1 configurations, optionally through dantto4k.

The ported satellite sequence and C/N curve are GPL-2.0-or-later. See
`BdaSpecials/TBS6812/NOTICE.md` and `COPYING` for origin and license details.
