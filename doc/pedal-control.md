# Pedal Control of the Charge Current

## Concept

While charging, the driver adjusts the charge current limit by tapping the pedals:

| Action | Effect |
|---|---|
| brake tap | limit = delivered current − 10 A |
| accelerator tap | limit = delivered current + 10 A |
| start of each charging session | limit = 100 A (default) |

- The base is the **delivered current** (EVSE present current, reported by the
  CCM in 0x5E5 B6..B7), because the driver sees only the delivered power. So a
  brake tap always has a visible effect, independent of whether the car, the
  charger or our limit is the bottleneck, and taps do not accumulate invisibly.
  An accelerator tap is only visible if our limit was the bottleneck.
  A tap during the current ramp-up acts on the momentary delivered current (e.g.
  a brake tap at 47 A gives a limit of 37 A). This is intended: it matches what
  the driver sees, and the limit can be raised again with accelerator taps.
- Taps are evaluated only during CurrentDemand (CCM progress 0x5E9) with a
  recent 0x5E5. Before (no current flows), taps are counted on the serial line
  as "ignored taps", but have no effect. This also serves the bring-up: the tap
  counters show that the pedals are recognized, without charging.
- A **tap** is a press which is released within 1 s. Longer presses are ignored,
  so holding a pedal or normal driving does not change the limit.
- Range 30 A … 200 A. 200 A is the EVMaximumCurrentLimit of the car, so it
  means "no reduction".
- The limit is applied by the MITM board to the target current 0x59C B4..B5
  (see [bench-testing.md](bench-testing.md)). The firmware only lowers the
  request of the car, never raises it.
- Session start = the CCM progress (0x5E9 B0 without bit 0) leaves idle (0).

Implementation: [src/pedalhandler.c](../src/pedalhandler.c), used by
[src/canhandler.c](../src/canhandler.c). The serial line shows once per second:
`limit: 100A  present: 0A  limited: n  accel: 0 released taps 0  brake: released taps 0  ignored taps: 0  sessions: 1`

Verified on the bench with the firmware (with pyPLC simulating a charger limited
to 150 A, the car requesting 175 A): accelerator taps raise the delivered current
in 10 A steps up to the 150 A of the charger, the next brake tap reduces it at
once to 140 A.

## Signals

Source: [hyundai_Ioniq28Motor.dbc](../../Ioniq28Investigations/CAN/hyundai_Ioniq28Motor.dbc)
(newest copy, 2025-07-10), verified with the traces.

| Pedal | CAN | Coding | DBC name | Cycle |
|---|---|---|---|---|
| Accelerator | 0x200 (VCU200) B4 | 0..255, pressed above 0x40 (25 %), released below 0x10 | `uwe_accpedal` | 10 ms |
| Brake | 0x394 (TCS13) B6 bit 7 | 1 = pressed | `DriverBraking` (same: B7 bit 4 `CF_DriBkeStat`) | 20 ms |

Notes:
- 0x200 B7 carries the accelerator pedal in % (B4 = 0x5E → B7 = 0x25 = 37 %).
- The DBC also contains `Brake_Pedal_Pos` and `Accel_Pedal_Pos`, but not
  assigned to any message (VECTOR__INDEPENDENT_SIG_MSG).
- `BrakeLight` (0x394 bit 11) never changed in the traces, not usable.
- If a signal is missing for more than 500 ms, a running press is discarded.

Verification with `IONIQ_PCAN_drive_fwd_neutral_drive_reverse_neutral.csv`
(driving): brake presses 1.8 … 5.4 s (no taps), two short accelerator blips
(420 ms, 340 ms, up to 37 %) which count as taps. Irrelevant for charging,
because the limit is reset at the session start.

**Open:** in the HPC charging trace, both pedals are used only before plug-in
and after the session, not during charging. Both messages are sent during
charging, but it is not yet verified that they react to the pedals while the
car is charging (not READY). → test in the car: press the pedals during
charging and watch the serial line.

## Test

On the bench (MITM board with the firmware, CCM, pyPLC), the vehicle simulation
sends pedal taps: `python tools/benchsession.py -d 62 --current 0:150 --taps 10:brake,15:brake,25:accel,35:brakelong`
gives EVTargetCurrent 100 → 90 → 80 → 90 A, the long brake press is ignored.
Also tested on the bench: the clamps at 30 A and 200 A, and that a limit above the
request of the car has no effect (the request of the car is forwarded).


Host test with the unchanged pedalhandler.c (MinGW gcc):

```sh
cd tools/pedaltest
gcc -Wall -Wextra -I../../src -o pedaltest.exe pedaltest.c ../../src/pedalhandler.c
./pedaltest.exe --selftest          # synthetic charging session, 13 checks
./pedaltest.exe <PCAN-View trace.csv>  # replay a real trace
```
