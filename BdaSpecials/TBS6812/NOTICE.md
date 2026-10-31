# Source attribution

The CXD2868ER/Freia satellite tuning sequence and CXD2857ER monitor definitions
are adapted from Till0196/coax, commit 2ef4838227f5c8f968f9acd74161c0437a165dc6:

- internal/coaxd/demod/cxd2857er/modes.go
- internal/coaxd/demod/cxd2857er/cn.go
- internal/coaxd/demod/cxd2857er/status.go
- internal/coaxd/demod/sony/chip.go
- internal/coaxd/demod/sony/freia.go and tuner.go (terrestrial RF packet)
- internal/coaxd/demod/sony/terrestrial.go and status.go (ISDB-T mode/monitors)
- internal/coaxd/demod/sony/cable.go (J.83B wakeup, IF and symbol-rate settings)
- internal/coaxd/pcie/tbs/tbs_windows.go (per-slot tuner addresses)
- internal/coaxd/demod/cxd2857er/tuner.go (shared reference clock constraints)

https://github.com/Till0196/coax/tree/2ef4838227f5c8f968f9acd74161c0437a165dc6

Copyright (c) Sony; TurboSight; 2026 Till0196. These adaptations are distributed
under GPL-2.0-or-later. See COPYING. No external firmware is included.

This extension adopts a vendor-initialized, active ISDB-T, ISDB-S or ISDB-S3 demodulator.
J.83B 5.6 Msym/s borrows the prepared ISDB-T core and restores its state on close.
It intentionally does not port the cold initialization, PLL, GPIO,
or direct satellite/terrestrial side transitions. ISDB-T retains the vendor-prepared 6 MHz
filter/IF coefficients and ports the per-channel terrestrial RF sequence only. It does not bypass conditional-access processing;
TVTest/EDCB handles conventional TS access, and the existing dantto4k/card path
handles S3 access.
