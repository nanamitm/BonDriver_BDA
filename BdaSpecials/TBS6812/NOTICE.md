# Source attribution

The CXD2868ER/Freia satellite tuning sequence and CXD2857ER monitor definitions
are adapted from Till0196/coax, commit 2ef4838227f5c8f968f9acd74161c0437a165dc6:

- internal/coaxd/demod/cxd2857er/modes.go
- internal/coaxd/demod/cxd2857er/cn.go
- internal/coaxd/demod/cxd2857er/status.go
- internal/coaxd/demod/sony/chip.go

https://github.com/Till0196/coax/tree/2ef4838227f5c8f968f9acd74161c0437a165dc6

Copyright (c) Sony; TurboSight; 2026 Till0196. These adaptations are distributed
under GPL-2.0-or-later. See COPYING. No external firmware is included.

This extension only adopts a vendor-initialized, active ISDB-S3 demodulator.
It intentionally does not port the cold initialization, PLL, GPIO, terrestrial,
or ISDB-S mode transitions. It does not bypass conditional-access processing;
the existing dantto4k/card path remains responsible for that.
