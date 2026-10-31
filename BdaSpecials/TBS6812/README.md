# TBS6812 extension for BonDriver_BDA

`TBS6812.dll` controls the TBS 6812 (PT4K) directly through the vendor driver's I2C property.
One BonDriver can receive ISDB-T, optical J.83 Annex B (5.6 Msym/s), ISDB-S and ISDB-S3, and
moves between them in about a second without restarting the card.

## Requirements

- Windows x64 with the TBS 6812 vendor driver installed
- Both inputs fed with mixed terrestrial and satellite signals
- A received BS signal: the vendor driver prepares each input on a BS carrier when it opens
- For ISDB-S3 only: `BonDriver_dantto4k.dll` with `passthroughSpaces` support, and a card reader

Without any satellite signal, see [Terrestrial only](#terrestrial-only).

## Setup

1. Place the files in the folder of your application, for example TVTest:

   ```
   BonDriver_TBS6812.dll              copy of BonDriver_dantto4k.dll
   BonDriver_TBS6812.ini              from config/
   tbs6812-all/
     BonDriver_TBS6812_Inner.dll      copy of BonDriver_BDA.dll
     BonDriver_TBS6812_Inner.ini      from config/
     TBS6812.dll
   ```

   From a build of this repository, `tools/deploy-tbs.ps1` does this:

   ```
   ./tools/deploy-tbs.ps1 -Destination <folder> -DanttoWrapperPath <BonDriver_dantto4k.dll>
   ```

2. Set `smartCardReaderName` in `BonDriver_TBS6812.ini` to receive ISDB-S3.
3. Close other applications that use the card, including ones that do not use BonDriver_BDA.
4. Select `BonDriver_TBS6812.dll` in the application and run a channel scan.

The first open sets both inputs to satellite mode (2,2) when no BonDriver_BDA receiver holds the card. This restarts
the card, once, and would interrupt an application that does not use BonDriver_BDA. If a receiver holds the card, the
mode is left as it is and the next free open tries again.

## Tuning spaces

| Space | Name | System | Stream |
| --- | --- | --- | --- |
| 0 | UHF | ISDB-T | MPEG-2 TS |
| 1 | Opticast | J.83 Annex B | MPEG-2 TS |
| 2 | BS | ISDB-S | MPEG-2 TS |
| 3 | CS110 | ISDB-S | MPEG-2 TS |
| 4 | BS4K8K | ISDB-S3 | MMT/TLV, converted to TS by dantto4k |

`passthroughSpaces=0,1,2,3` tells dantto4k which spaces to pass on unchanged. Keep it in step with the
INI if you add or remove spaces. Conditional access of the TS spaces stays with your application.

Two receivers can run at once, in any combination of systems. Each one takes a free input automatically.

## Settings

In the `[TBS6812]` section of `BonDriver_TBS6812_Inner.ini`:

| Key | Default | Meaning |
| --- | --- | --- |
| `DeliverySystem` | | `"ISDB-T/J.83B/ISDB-S/ISDB-S3"` enables switching. Each `[TuningSpaceNN]` then names its own system |
| `LockTimeoutMs` | 3000 | How long a tune waits for a lock |
| `AutoBoardMode` | 1 | Set both inputs to satellite mode when opening. 0 leaves the mode to `tbs6812-board-mode.exe write 2 2` |
| `DirectSettleMs` | 300 | Wait after a lock before buffers are purged, which keeps stale data out of the stream |
| `BootstrapSatelliteRFKHz` | 11727480 | BS carrier for preparing an input when the first channel is ISDB-T or J.83B |
| `BootstrapSatelliteTSID` | 16400 | TSID of that carrier (0x4010, BS01/TS0) |

## Terrestrial only

Use `config/BonDriver_TBS6812_T.ini` directly with a renamed `BonDriver_BDA.dll`, without dantto4k. It receives
ISDB-T and J.83B. The input must be in terrestrial mode (`tbs6812-board-mode.exe write 1 1`), and the J.83B
preparation needs a terrestrial carrier: set `BootstrapFrequencyKHz` to one you receive.

**This setup cannot coexist with `BonDriver_TBS6812.dll`.** Opening that BonDriver rewrites the mode to 2,2, after
which this one cannot open until you write the terrestrial mode again. Do not deploy both, or set `AutoBoardMode=0` in
`BonDriver_TBS6812_Inner.ini` and manage the mode yourself. Each mode change restarts the card and, if the mode is
kept in non-volatile memory, wears it, so avoid switching back and forth.

## Behavior to expect

- The first channel after opening takes longer, 2 to 8 seconds, because the vendor driver prepares the input.
  Later changes take about 0.5 to 1.4 seconds.
- Closing returns the input to the state the vendor driver prepared. After an abnormal exit, the next open
  restores the demodulator.
- `TBS6812.log` next to `TBS6812.dll` records preparation, switching, timeouts and recovery.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| The BonDriver does not open or tune | An input in terrestrial mode cannot be used: run `tbs6812-board-mode.exe` and expect `modes=2,2`. The mode changes only while the card is free, and all inputs may also be in use |
| The first tune fails | The BS carrier must lock. Check the signal, or change the bootstrap settings |
| A channel never locks | Look for `direct tune timed out` in the log, and check the signal |
| ISDB-S3 has no picture | Check `smartCardReaderName` and the reader |
| ISDB-T or BS has no picture | Check that its space is in `passthroughSpaces` and your application's conditional access |

Writing the mode restarts the card and interrupts every receiver on it. `tbs6812-board-mode.exe` refuses while a
BonDriver_BDA receiver holds an input, but cannot detect applications that do not use BonDriver_BDA.

## Limits

Not verified: EDCB recording, long recordings, whether board modes survive a PC restart, and recovery from an
I2C failure during a switch. Switching drives the chip outside what the vendor driver expects, so a future
driver version may behave differently.

## License

GPL-2.0-or-later. See `COPYING`. Sources are listed in `NOTICE.md`. No firmware is included.
