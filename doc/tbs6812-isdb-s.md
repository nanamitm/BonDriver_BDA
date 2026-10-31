# TBS6812 conventional ISDB-S support

Current setup: ISDB-S is part of [tbs6812-all.md](tbs6812-all.md). This page records the control details. `DeliverySystem="ISDB-S"` still selects it alone.

Implemented on `codex/tbs6812-isdb-s`, based on merged master `2a3871e`.
The earlier S3 work was merged through PR #1 before creating this branch.

## Setup

Build with `tools/build-tbs.ps1`. In a staging directory, place:

- `x64/Release_Static/BonDriver_BDA.dll`, renamed `BonDriver_TBS6812_S.dll`
- `x64/Release_Static/TBS6812.dll`
- `config/BonDriver_TBS6812_S.ini` with the same basename as the BonDriver
- the TBS6812 COPYING and NOTICE.md files when distributing binaries

Select this BonDriver directly in TVTest/EDCB. It supplies conventional 188-byte
TS; use the application's existing B-CAS processing. Do not route it through
the current TLV-only dantto4k input. Existing S3 configurations keep using
BonDriver_dantto4k and their inner BonDriver_BDA_4k.

The INI selects `[TBS6812] SatelliteSystem="ISDB-S"` and symbol rate 28860.
Omitting SatelliteSystem retains the existing ISDB-S3 behavior. Unknown values
are rejected. The template automatically selects an available physical slot;
adding ` 0` or ` 1` to both tuner/capture friendly names pins that slot. The same
allocation/exclusion mechanism covers both delivery systems, with two receivers
in total per card.

Use absolute TSIDs in the channel list. The CS list contains the TSIDs observed
in the received NIT on 2026-10-31. Generic `ChannelSettingsAuto="CS110"` leaves
TSID unspecified and is unsuitable here. Relative indices 0..7 work after warm
adoption, but the initial vendor bootstrap requires an absolute TSID; the vendor
property did not lock with relative index 0 in the hardware test.

## Control differences

The first tune still lets the Windows driver establish the selected core and
clock. Adoption checks core 0x0c and TS output for S, versus core 0x0d and TLV
output for S3. Later S tunes use:

- Freia ISDB-S filter settings and IF 950..2150 MHz
- SLV-T bank C0, E9..EB for absolute TSID / relative TS index selection
- bank 00, C3 to pause/resume TS around RF tuning and soft reacquisition
- bank A0, 12 bit 6 for lock
- bank A1, 10..12 and the conventional satellite C/N curve

Close restores the initial carrier, selection mode and output settings. No cold
reset, board-wide mode write or shared clock reinitialization is introduced.
Changing between S and S3 across opens uses vendor bootstrap when the prepared
core differs. Direct transitions within one open session are outside this change.

ExperimentalWarmStart remains opt-in. Version 2 cache records include the
delivery system and reject records for the other system. Legacy version 1
records are accepted only for S3. Both frontends should use the same absolute
cache directory so each physical slot has one current record. The record is
consumed before reuse and republished only after successful locked restoration.
Older S3 binaries reject version 2 and safely take the vendor bootstrap path.

## Validation on 2026-10-31

Release_Static x64 build and `tools/test-tbs6812.cmd` passed. Mock checks cover
S-specific RF filters, lock register, C/N conversion, absolute/relative selection,
restoration, rejection before RF writes, cross-system cache isolation and legacy
S3 cache compatibility, alongside the existing S3/identity checks.

Hardware probe results (SetChannel duration, excluding OpenTuner):

| Operation | Observed time |
| --- | --- |
| First BS vendor bootstrap | 4.1..4.8 s |
| BS same-carrier TSID change | about 215 ms |
| BS different-carrier change | about 216 ms |
| BS to CS / CS change | about 201..250 ms |
| Cached BS reopen | about 259 ms |

BS captures identified TSIDs 4010 (BS Asahi), 4011 (BS-TBS), 4030 (WOWOW),
4090 (BS11); CS captures identified 6020 and 7040. S/S and S/S3 simultaneous
reception, S3 right/left carriers, close/reopen and S-to-S3-to-S handback passed.
These are short API/stream tests, not TVTest visual confirmation or a long
recording stability test. Scrambled conventional TS packets are expected before
the application's B-CAS processing.

Some captures include stale data or a discontinuity at the selection boundary.
Removing the first 3 MiB from the affected diagnostic samples gave zero sync,
TEI and continuity errors in the remainder. This is evidence that the observed
errors were at startup, not a proposed fixed-size discard algorithm. Initial old
data filtering remains a shared S/S3 follow-up; this change does not solve it.

The reproducible opt-in two-slot test is:


It stages the current binaries and pinned configurations under work/, captures
S/S and mixed-system reception, and checks probe success/normal unloading.
Inspect the saved TS/TLV separately for identity and packet continuity; process
success alone does not establish stream correctness. Logs/captures remain under
`work/isdb-s-results`. Production TVTest/EDCB files have not been replaced.
