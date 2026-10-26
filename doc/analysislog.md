# CAN Analysis — Ioniq 28 kWh Charge Power Reducer

## Goal

Find the CAN signal by which the vehicle tells the CCM (charge control module)
the charge target current, so that the STM32 device between CAN1 (vehicle side)
and CAN2 (CCM side) can reduce it.

**Result:** the target current is **0x59C B4..B5 (LE16, 0.1 A)**. The CCM
forwards it 1:1 as EVTargetCurrent to the charger. The firmware limits this
value (see [pedal-control.md](pedal-control.md)). The bench tests which prove
this are described in [bench-testing.md](bench-testing.md).

---

## Bus characteristics

Source trace: `C:\UwesTechnik\Ioniq28Investigations\CAN\IONIQ_PCAN_leave_car_while_drive_ready_beep.csv`

- **86 distinct CAN IDs** on the bus
- ~2 240 frames/s while the ECUs are awake
- Bus speed: 500 kBaud

---

## Known decoded signals relevant to charging

| CAN ID | DBC name | Signal | Scale / unit | Notes |
|--------|----------|--------|--------------|-------|
| 0x542  | `uwe542` | `uwe542_0_SocDisp` | 0.5 %/LSB, 0–100 % | HV battery SOC display value |
| 0x542  | `uwe542` | `uwe542_78_UCCS`   | 0.1 V/LSB, 0–500 V | CCS inlet voltage, measured by the vehicle (not evaluated by the CCM) |
| 0x595  | `uwe595` | `uwe595_IBatt`     | 0.1 A/LSB, ±1000 A | HV battery current |
| 0x595  | `uwe595` | `uwe595_UBatt`     | 0.1 V/LSB, 0–500 V | HV battery voltage |
| 0x596  | `uwe596` | `remainingChTDC`   | 1 min/LSB           | Remaining DC charge time (not used by the CCM) |
| 0x596  | `uwe596` | `remainingChTAC`   | 1 min/LSB           | Remaining AC charge time |
| 0x59C  | —        | target voltage     | B2..B3, 0.1 V/LSB (LE16) | battery voltage during precharge, max voltage 412.8 V during current demand |
| 0x59C  | —        | target current     | B4..B5, 0.1 A/LSB (LE16) | forwarded by the CCM as EVTargetCurrent |
| 0x59E  | —        | remaining time to full SOC | B0..B1, 1 s/LSB (LE16) | forwarded as RemainingTimeToFullSoC |
| 0x59E  | —        | remaining time to bulk SOC | B2..B3, 1 s/LSB (LE16) | forwarded as RemainingTimeToBulkSoC |
| 0x59E  | —        | maximum power      | B6..B7, 10 W/LSB (LE16) | forwarded as EVMaximumPowerLimit (car: 98 kW) |

Instantaneous charge power = `uwe595_IBatt` × `uwe542_78_UCCS`.

---

## CCM hardware — controller

The CCM main controller is a **Freescale MPC5605BMLQ** (PowerPC e200 / Power
Architecture), 144-pin. Memory map:

| Region | Address range | Size |
|--------|--------------|------|
| Code flash | 0x00'0000 – 0x0B'FFFF | 768 KB |
| Shadow flash | 0x20'0000 – 0x20'3FFF | 16 KB |
| Data flash | 0x80'0000 – 0x80'FFFF | 64 KB |

Dumps available in `C:\UwesTechnik\Ioniq28Investigations\CCM_ChargeControlModule_PLC_CCS\`.
JTAG debug port (CON3) is unprotected; memory was read out directly with a
debugger after bridging CON12 to suppress the watchdog.

---

## CCM CAN interface

Details and the bench simulation: `CCM_ChargeControlModule_PLC_CCS\readme.md`,
chapter "Running a complete charging session on the bench".

### CCM receives (vehicle → CCM)

Needed for a complete session (SLAC until CurrentDemand):

| ID | Needed for | Content |
|----|-----------|---------|
| 0x57F | SLAC | static `94 76 92 00 07 20 00 05`, meaning unknown |
| 0x597 | SLAC | B0 bit 7 toggle bit; B7 `00` → `02` at power delivery |
| 0x599 | SLAC | B2 SOC (0.5 %), forwarded as EVRESSSOC (integer %, the half percent is dropped); B7 bit 0 CCS trigger |
| 0x5A3 | ChargeParameterDiscovery | static `63 30 48 00 00 00 00 00` (B1..B2 = 18480, maybe EVEnergyRequest in Wh) |
| 0x59C | PreCharge, CurrentDemand | target voltage and target current, see above |

Needed for a real charger: 0x59E (remaining times and maximum power, see above;
without it, the CCM announces a maximum power of 0).

Sent by the car, but not used by the CCM: 0x5D7 (handshake flags
CCS_firstTrigger, CcsStart, ConnectorLockConfirm, PreChCnf, ContactorsClosed;
B2..B3 and B7 mirror the target current of 0x59C; B4..B5 mirrors the remaining
time to bulk SOC of 0x59E), 0x542 (inlet voltage), and all other messages.

### CCM transmits (CCM → vehicle), all 100 ms

| ID | Content (LE16 values) |
|----|---------|
| 0x5E5 | B0..B1 EVSE maximum voltage (0.1 V), B2..B3 EVSE maximum current (0.1 A), B4..B5 EVSE present voltage (0.1 V), B6..B7 EVSE present current (0.1 A) |
| 0x5E6 | B0 bit 3 cable check running, B0 bit 7 current demand running, B1 bit 2 cable check done; B6..B7 likely EVSE maximum power (10 W) |
| 0x5E8 | B2..B3 likely EVSE minimum current (0.1 A), B4..B5 likely EVSE minimum voltage (0.1 V), B6 EVSEStatusCode (01 Ready, 04 IsolationMonitoringActive, 00 NotReady; D0/D1 before an abort) |
| 0x5E9 | B0 bit 0 alive toggle, bits 1..7 CCM_ChargeProgress: 1 charge parameters known, 3 CableCheck, 7 PreCharge, 15 PowerDelivery, 31 CurrentDemand, 63 PowerDeliveryStop, 127 WeldingDetection |

### Charging session sequence (car trace with Alpitronic charger)

1. Plug-in / 5% PWM → `5D7_1_CCS_firstTrigger = 1`
2. +600 ms: CCS trigger (0x599 B7 bit 0), `5D7_1_CcsStart = 1`
3. +6.9 s: CCM reports `CCM_CCS_TCP_connected = 1` (on 0x5Ex)
4. +8.5 s: `CCM_ChargeProgress = 1` (charge parameters known), vehicle sets `5D7_0_ConnectorLockConfirm`
5. `CCM_ChargeProgress` → CableCheck (3); the inlet voltage (0x542) rises to 500 V and comes down after the cable check
6. CCM reports cable check done (0x5E6 B1 bit 2); the vehicle requests 1 A at battery voltage in 0x59C
7. `CCM_ChargeProgress` → PreCharge (7); vehicle sets `5D7_1.1_PreChCnf` when the voltage is reached
8. `CCM_ChargeProgress` → PowerDelivery (15); vehicle sets `5D7_1.4_ContactorsClosed`
9. `CCM_ChargeProgress` → CurrentDemand (31); the vehicle ramps the target current in 0x59C up
   (~1 A per 100 ms, reaching 148 A after 15 s on Alpitronic HYC300). The CCM forwards it without own ramp.
10. On termination: `CCM_ChargeProgress` → PowerDeliveryStop (63), then WeldingDetection (127)

---

## CCM firmware analysis

Binary search of the code flash dump (`readout_codeflash_CCM_ioniq_00_33_79.s19`,
768 KB, MPC5605B PowerPC) for CAN IDs in LE16/BE16/shifted encodings.

**CAN receive handler table at 0x1AAFA:** a contiguous block of LE16 CAN IDs:

```
0x1AAFA: 0x0590
0x1AB14: 0x0591
0x1AB18: 0x0592
0x1AB1C: 0x0593
0x1AB20: 0x0595
0x1AB24: 0x0594
0x1AB28: 0x0597
0x1AB2C: 0x0596
```

All eight IDs 0x590–0x597 appear together — very likely a CAN receive dispatch
table. The interleaved values between IDs are likely handler offsets or message
descriptors. (0x59C, 0x59E and 0x5A3 are not in this block; where they are
handled in the firmware is not analyzed.)

---

## HPC charging trace

File: `C:\UwesTechnik\IoniqMotorCAN\IoniqMotorCAN\Traces\ioniq2018_motorCAN_500k_CCS_charging_on_HPC\ioniq2018_motorCAN_500k_HPC.asc`
(Vector ASC format, 274 MB, recorded 2022-07-22)

| | |
|---|---|
| Duration | 19.2 min (1150 s) |
| SOC range | 34% → 88% |
| Peak charge current | −174 A |
| Peak charge power | −68 kW |
| Battery voltage range | 346 V → 391 V |

### Current taper profile

| Event | t (s) | t (min) | SOC | IBatt (A) | UBatt (V) | Power (kW) |
|-------|--------|---------|-----|-----------|-----------|------------|
| Peak | 2759 | 46.0 | 34.5% | −174 | 358 | −62 |
| Taper start (Step 1) | 3421 | 57.0 | 77.5% | −170 | 391 | −67 |
| Step 2 | 3426 | 57.1 | 78.0% | −165 | 391 | −65 |
| Step 3 | 3431 | 57.2 | 78.0% | −160 | 391 | −62 |
| Step 4 | 3436 | 57.3 | 78.5% | −156 | 391 | −61 |
| Step 5 | 3441 | 57.3 | 79.0% | −150 | 391 | −59 |
| Step 6 | 3449 | 57.5 | 79.5% | −146 | 391 | −57 |
| Step 7 | 3454 | 57.6 | 79.5% | −141 | 391 | −55 |
| Step 8 | 3459 | 57.7 | 80.0% | −137 | 391 | −54 |
| Step 9 | 3469 | 57.8 | 80.5% | −133 | 391 | −52 |
| Step 10 | 3474 | 57.9 | 80.5% | −129 | 391 | −50 |
| Step 11 | 3484 | 58.1 | 81.0% | −124 | 391 | −49 |
| Step 12 | 3494 | 58.2 | 81.5% | −120 | 391 | −47 |
| Step 13 | 3503 | 58.4 | 82.0% | −116 | 391 | −45 |
| Step 14 | 3508 | 58.5 | 82.0% | −112 | 391 | −44 |
| Step 15 | 3517 | 58.6 | 82.5% | −108 | 391 | −42 |
| Step 16 | 3528 | 58.8 | 83.0% | −104 | 391 | −41 |
| **Big drop 1** | 3533 | 58.9 | 83.5% | **−82** | 389 | **−32** |
| **Big drop 2** | 3538 | 59.0 | 83.5% | **−55** | 389 | **−22** |
| Second plateau | 3732 | 62.2 | 88.0% | −51 | 388 | −20 |
| Session end (trace) | 3806 | 63.4 | 88.0% | ~0 | 386 | ~0 |

**Pattern summary:**
- 34.5% → 77.5%: constant ~174 A (≈62 kW), no stepping
- 77.5% → 83.5%: regular ~4 A steps every 5–10 s (16 steps over 2 min)
- 83.5%: two large jumps (−26 A each), current halves in 10 s
- 83.5% → 88%: holds at ~51 A (≈20 kW) for ~3 min
- 88%: drops to zero — trace ends here (actual termination at 94% not captured)

### Target current in the trace

The target current of the vehicle (0x59C B4..B5, mirrored in 0x5D7 B2..B3 in
0.1 A and 0x5D7 B7 in 1 A) follows the battery current within 1–3 A during the
whole taper:

| Event | t (s) | SOC | 0x5D7 B7 (A) | Actual IBatt |
|-------|--------|-----|-------------|--------------|
| Peak | 2759 | 34.5% | 173 | −174 A |
| Step 1 | 3421 | 77.5% | 168 | −170 A |
| Step 2 | 3426 | 78.0% | 163 | −165 A |
| Step 3 | 3431 | 78.0% | 158 | −160 A |
| Step 4 | 3436 | 78.5% | 153 | −156 A |
| Step 5 | 3441 | 79.0% | 149 | −150 A |
| Step 6 | 3449 | 79.5% | 144 | −146 A |
| Step 7 | 3454 | 79.5% | 139 | −141 A |
| Step 8 | 3459 | 80.0% | 137 | −137 A |
| Step 9 | 3469 | 80.5% | 132 | −133 A |
| Step 10 | 3474 | 80.5% | 127 | −129 A |
| Step 11 | 3484 | 81.0% | 123 | −124 A |
| Step 12 | 3494 | 81.5% | 119 | −120 A |
| Step 13 | 3503 | 82.0% | 115 | −116 A |
| Step 14 | 3508 | 82.0% | 113 | −112 A |
| Step 15 | 3517 | 82.5% | 108 | −108 A |
| Step 16 | 3528 | 83.0% | 102 | −104 A |
| Big drop 1 | 3533 | 83.5% | 64 | −82 A |
| Big drop 2 | 3538 | 83.5% | 55 | −55 A |
| Session end | 3737 | 88.0% | 0 | 0 A |

So the taper (including the 4 A steps) is made by the vehicle side; the charger
follows the EVTargetCurrent with a short delay.

### Other observations in the trace

- **0x57F:** static `94 76 92 00 07 20 00 05` during charging; changes only after the session end.
- **0x599:** B2 = SOC (same value and scale as `uwe542_0_SocDisp`). B7 = `0x03` during charging → `0x81` at session stop → `0x00` after.
- **0x597:** B0 bit 7 toggle bit. B1 decreases in coarse steps during the taper
  (`BE` constant phase → `46` → `41` → `3C` → `37` → `32` → `28`), meaning unknown;
  it is not the target current (the CCM follows 0x59C). B7: `02` before the taper,
  `04` during the taper, `80` at session stop.
- **0x590, 0x592, 0x594:** no relation to the current (0x590 noise-like values,
  0x592 static with a 2-bit counter in B7, 0x594 B5 another SOC representation).
- **0x5D7 B4..B5:** remaining time to bulk SOC in s, decreasing in steps of 60 s
  (840 s at the session start); mirror of 0x59E B2..B3.

---

## Intervention

The firmware modifies 0x59C on the way from CAN1 (vehicle) to CAN2 (CCM): if the
target current B4..B5 exceeds the limit, it is replaced by the limit. The CCM
forwards the value 1:1, also as steps; 0x59C has no visible counter or checksum.
The limit is set by the driver with the pedals ([pedal-control.md](pedal-control.md)).

The car accepts a lower current than requested (some chargers also deliver less
than requested).

**Power reduction examples:**

| Target power | Target current (at ~385 V) |
|---|---|
| 50 kW | ~130 A |
| 40 kW | ~104 A |
| 25 kW | ~65 A |

A second possible lever is the EVMaximumPowerLimit in 0x59E B6..B7; whether a
charger honors it is not tested.

---

## Open questions

- Is there a separate "terminate charging" flag, or does setting the current to
  zero serve as termination? Candidates: 0x599 B7 (`0x03` → `0x81` at stop),
  0x597 B7 (`0x80` at stop).
- What does 0x57F contain? (needed for SLAC, static during charging)
- Which bytes inside the needed messages are really evaluated by the CCM (e.g.
  whether the changes of 0x597 / 0x599 during the session are needed)?
- Source of EVMaximumCurrentLimit (200.0 A; maybe 0x599 B3..B4 = 0x07D0).
- Meaning of 0x597 B1.
