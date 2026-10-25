# CAN Analysis Log — Ioniq 28 kWh Charge Power Reducer

## Goal

Identify the CAN message(s) by which the BMS communicates the charge target
current or target power to the CCM/OBC, and the SOC at which CCS charging
terminates (94%). Once identified, the STM32 device will sit between CAN1
(vehicle side) and CAN2 (charger side) and modify those signals to reduce the
charge power setpoint.

---

## Bus characteristics

Source trace: `C:\UwesTechnik\Ioniq28Investigations\CAN\IONIQ_PCAN_leave_car_while_drive_ready_beep.csv`

- **86 distinct CAN IDs** on the bus
- Active traffic only during ~28 seconds of that recording (seconds 0–27),
  ~2 240 frames/s. The rest of the 28-minute file is ECUs sleeping.
- Bus speed: 500 kBaud

---

## Known decoded signals relevant to charging

| CAN ID | DBC name | Signal | Scale / unit | Notes |
|--------|----------|--------|--------------|-------|
| 0x542  | `uwe542` | `uwe542_0_SocDisp` | 0.5 %/LSB, 0–100 % | HV battery SOC display value |
| 0x542  | `uwe542` | `uwe542_78_UCCS`   | 0.1 V/LSB, 0–500 V | CCS charge station voltage |
| 0x595  | `uwe595` | `uwe595_IBatt`     | 0.1 A/LSB, ±1000 A | HV battery current (VCU measured, sent back to BMS) |
| 0x595  | `uwe595` | `uwe595_UBatt`     | 0.1 V/LSB, 0–500 V | HV battery voltage |
| 0x596  | `uwe596` | `remainingChTDC`   | 1 min/LSB           | Remaining DC charge time |
| 0x596  | `uwe596` | `remainingChTAC`   | 1 min/LSB           | Remaining AC charge time |

Instantaneous charge power = `uwe595_IBatt` × `uwe542_78_UCCS` (both from bus,
same timebase — no external measurement needed).

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

## CCM CAN message directions

Source: `CCM_ChargeControlModule_PLC_CCS\readme.md`, section "CAN Communication".

### CCM transmits (vehicle bus → CCM side output)

| ID | Cycle |
|----|-------|
| 0x5E5 | 100 ms |
| 0x5E6 | 100 ms |
| 0x5E8 | 100 ms |
| 0x5E9 | 100 ms |

### CCM receives (vehicle bus → CCM input)

Minimum set required to make the CCM start SLAC/PLC communication:

| ID | Known content |
|----|--------------|
| **0x57F** | unknown |
| **0x597** | toggle bit + other info |
| **0x599** | CCS trigger bit (byte 7, bit 0); SOC in byte 2 |

Additional messages involved in the charging session sequence:

| ID | Known signals |
|----|--------------|
| **0x5D7** | `5D7_1_CCS_firstTrigger`, `5D7_1_CcsStart`, `5D7_4_remainingTimeBulk`, `5D7_0_ConnectorLockConfirm`, `5D7_1.1_PreChCnf`, `5D7_1.4_ContactorsClosed` |
| **0x542** | `542_78_UCCS` — CCS inlet voltage (already known) |

### Charging session sequence (timing from Alpitronic trace)

1. Plug-in / 5% PWM → `5D7_1_CCS_firstTrigger = 1`
2. +596 ms: `CCS_COMMS_TRIGGER = 1`
3. +600 ms: `5D7_1_CcsStart = 1`
4. +6.9 s: CCM reports `CCM_CCS_TCP_connected = 1` (on 0x5Ex)
5. +8.5 s: `CCM_ChargeProgress = 1` (connector lock request)
6. `5D7_0_ConnectorLockConfirm = 1`
7. `CCM_ChargeProgress` → CableCheck (3); `542_78_UCCS` ramps to 500 V
8. After cable check, voltage ramps down; when < 6 V → PreCharge
9. `5D7_1.1_PreChCnf` confirms precharge
10. `CCM_ChargeProgress` → PowerDeliveryStart, then CurrentDemand
11. `5D7_1.4_ContactorsClosed` confirms contactors closed
12. Current demand loop begins; CCM ramps EVTargetCurrent autonomously
    (~1 A/message, reaching 148 A after 15 s on Alpitronic HYC300)
13. `5D7_4_remainingTimeBulk` decrements each minute
14. On termination: `CCM_ChargeProgress` → PowerDeliveryStop (63),
    then WeldingDetection (127)

**Key observation:** The CCM generates the `EVTargetCurrent` (sent to the
charger via V2G/PLC) by itself — it ramps up autonomously. The vehicle must
communicate a **maximum current limit** somewhere in the received messages;
that limit is what this project needs to intercept and reduce.

---

## Suspect messages for charge target current / power

The DBC does not decode a charge current setpoint signal. Based on the
now-known CCM receive set, the primary suspects are:

| ID | Direction | Rationale |
|----|-----------|-----------|
| **0x597** | vehicle → CCM | required to start charging; carries toggle + unknown fields |
| **0x599** | vehicle → CCM | carries SOC; likely also carries current limit |
| **0x57F** | vehicle → CCM | content unknown; required for SLAC trigger |
| 0x579 | vehicle → CCM (DBC name VCU579) | unidentified VCU message in same range |
| 0x57A, 0x57B | vehicle → CCM (DBC names VCU57A/B) | unidentified; firmware analysis inconclusive |

---

## CCM firmware analysis — summary

Binary searched the code flash dump (`readout_codeflash_CCM_ioniq_00_33_79.s19`,
768 KB, MPC5605B PowerPC) for known and suspect CAN IDs in LE16/BE16/shifted
encodings. Source script:
`C:\Users\uwemi\AppData\Local\Temp\claude\...\scratchpad\search_can_ids.py`

**Sanity check passed:** All known IDs (0x542, 0x595, 0x596, 0x549) found. ✓

**Notable finding — CAN receive handler table at 0x1AAFA:**

Dumping context around the 0x592 LE16 hit revealed a contiguous block of LE16
CAN IDs at even-aligned addresses:

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

All eight IDs 0x590–0x597 appear together — this is almost certainly a CAN
receive dispatch table. The interleaved values between IDs are likely handler
offsets or message descriptors.

**0x57A and 0x57B hits:** The small number of hits (1 and 2 respectively) that
appeared significant turned out to be inside instruction code sequences, not
data tables — the bytes `05 7A` and `05 7B` appear as operand+opcode in
MPC5605B VLE instruction encoding. These IDs have no confirmed data-table
presence in the code flash.

**Firmware analysis suspended** pending live CCS recording.

---

## Existing HPC charging trace

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

Analysis script: `scratchpad/current_taper.py`

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

The 16 regular 4 A steps are the primary target for byte correlation.

### Byte correlation results

Analysis script: `scratchpad/byte_correlate.py`

#### 0x57F — **ruled out**
All 8 bytes completely static throughout the taper (value `94 76 92 00 07 20 00 05`).
Only changes after the session ends (Steps 19–22 at 88% when current is already 0).

#### 0x599 — SOC confirmed, no current limit
- **B2** increments by 1 (= 0.5% SOC) at each step, exactly matching the SOC from 0x542.
  Confirmed: `uwe542_0_SocDisp` and 0x599 B2 encode the same SOC with identical scale.
- B7 = `0x03` throughout charging → `0x81` at session stop → `0x00` after.
  Looks like a charge-phase status byte.
- No byte carries the current limit in this message.

#### 0x597 — **primary candidate: B1**

B0 bit 7 is the toggle bit (alternates `0x05` ↔ `0x85` each cycle, as stated in the readme).

**B1 decrements monotonically during the taper:**

| Event | t (s) | SOC | B1 (hex) | B1 (dec) | IBatt (A) | B1×2.5 (A) |
|-------|--------|-----|----------|----------|-----------|------------|
| Constant phase | 2759 | 34.5% | `BE` | 190 | −174 | 475 (*) |
| Step 1 — taper start | 3421 | 77.5% | `46` | 70 | −170 | **175** |
| Step 2 | 3426 | 78.0% | `41` | 65 | −165 | **163** |
| Steps 3–7 | — | 78–80% | `41` | 65 | — | — |
| Step 8 | 3459 | 80.0% | `3C` | 60 | −137 | 150 |
| Step 12 | 3494 | 81.5% | `37` | 55 | −120 | 138 |
| Step 17 | 3533 | 83.5% | `32` | 50 | −82 | 125 |
| Step 19 | 3732 | 88.0% | `28` | 40 | −51 | 100 |
| Session end | 3737 | 88.0% | `28` | 40 | 0 | — |

(*) `0xBE`=190 during the constant phase — likely a "full-power / no-limit" code rather
than a scaled current value, since 190 × 2.5 = 475 A makes no physical sense.

**Key observation:** B1 does NOT change at every 4 A IBatt step. It changes only ~6 times
while IBatt steps 19 times. Interpretation: the BMS sends a coarser current limit every
~0.5–1% SOC, and the CCM ramps its own EVTargetCurrent down in 4 A increments to reach
the new target. The 4 A micro-steps are the CCM's internal ramp algorithm.

**Scale hypothesis:** `B1 × 2.5 A/LSB` fits the first two taper points (175 A, 163 A).
Accuracy degrades at higher SOC, which could mean the CCM has not yet fully ramped down
to the latest target when the next B1 update arrives.

B7 in 0x597 appears to be a charge-state flag: `0x02` (pre-taper), `0x04` (taper active),
`0x80` (session stopping).

### Extended byte correlation — 0x590–0x594 and CCM outputs

#### 0x590, 0x592, 0x594 — receive-table messages

| ID | Verdict | Notes |
|----|---------|-------|
| 0x590 | Ruled out | B2/B4/B6 oscillate 0x0F–0x12 / 0x93–0x94 without trend — likely temperature or sensor noise |
| 0x591 | Not in trace | — |
| 0x592 | Ruled out | B0–B6 static; B7 cycles 04/44/84/C4 = 2-bit rolling counter |
| 0x593 | Not in trace | — |
| 0x594 | Ruled out | B5 increments like SOC (another SOC representation); B6 oscillates 0–4; no current limit |

#### 0x5D7 — **primary finding**

Direction: **VCU → CCM** (confirmed by session sequence: carries `CCS_firstTrigger`, `ContactorsClosed`, etc.)

**B7 = charge current setpoint, 1 A/LSB:**

| Event | t (s) | SOC | B7 (hex) | B7 (dec = A) | Actual IBatt |
|-------|--------|-----|----------|-------------|--------------|
| Peak | 2759 | 34.5% | `AD` | 173 | −174 A |
| Step 1 | 3421 | 77.5% | `A8` | 168 | −170 A |
| Step 2 | 3426 | 78.0% | `A3` | 163 | −165 A |
| Step 3 | 3431 | 78.0% | `9E` | 158 | −160 A |
| Step 4 | 3436 | 78.5% | `99` | 153 | −156 A |
| Step 5 | 3441 | 79.0% | `95` | 149 | −150 A |
| Step 6 | 3449 | 79.5% | `90` | 144 | −146 A |
| Step 7 | 3454 | 79.5% | `8B` | 139 | −141 A |
| Step 8 | 3459 | 80.0% | `89` | 137 | −137 A ← exact |
| Step 9 | 3469 | 80.5% | `84` | 132 | −133 A |
| Step 10 | 3474 | 80.5% | `7F` | 127 | −129 A |
| Step 11 | 3484 | 81.0% | `7B` | 123 | −124 A ← exact |
| Step 12 | 3494 | 81.5% | `77` | 119 | −120 A ← exact |
| Step 13 | 3503 | 82.0% | `73` | 115 | −116 A |
| Step 14 | 3508 | 82.0% | `71` | 113 | −112 A |
| Step 15 | 3517 | 82.5% | `6C` | 108 | −108 A ← exact |
| Step 16 | 3528 | 83.0% | `66` | 102 | −104 A |
| Big drop 1 | 3533 | 83.5% | `40` | 64 | −82 A |
| Big drop 2 | 3538 | 83.5% | `37` | 55 | −55 A ← exact |
| Session end | 3737 | 88.0% | `00` | 0 | 0 A |

The match is within 1–3 A throughout the entire taper. The two "big drops" at 83.5%
where B7 jumps −38 and then the current follows are consistent with the CCM ramping
smoothly to the new setpoint.

**B2+B3 (LE16)**: 0x06C5=1733 → 0x0000=0, monotonically decreasing — `remainingTimeBulk`
in seconds (28.9 min remaining at 34.5% SOC).

**B4**: coarse plateaus 0xDC→0xB4→0x78→0x3C→0x00 (220→180→120→60→0 A) — likely the
EVSEMaxCurrent reported by the charger via V2G, sent back on CAN.

#### CCM outputs (0x5E5–0x5E9)

| ID | Verdict | Notes |
|----|---------|-------|
| 0x5E5 | No clear current signal | B6 is a fast-running counter; B4 decreases slowly but scale unclear |
| 0x5E6 | Static during taper | Changes only at session end |
| 0x5E8 | Static during taper | B4+B5 = 0x05DC = 1500 constant — possibly max voltage×10 (150V?) or power limit |
| 0x5E9 | Toggle only | B0 alternates 3E/3F (alive counter); jumps to 7F at session end |

---

## Intervention plan

**Target byte: 0x5D7 B7, scale 1 A/LSB.**

0x5D7 is sent by the VCU and received by the CCM. The STM32 device sits on the
CAN bus between them and intercepts every 0x5D7 frame. To reduce charge power:

1. Receive the original 0x5D7 frame from the VCU.
2. Read B7 (the current setpoint in A).
3. If B7 exceeds the desired limit, cap it (e.g. `B7 = min(B7, 0x68)` for ≤ 104 A ≈ 40 kW at 385 V).
4. Recompute the CRC/checksum if present (B0 appears to carry charge-state flags, not a checksum).
5. Re-transmit the modified frame to the CCM.

**Power reduction examples:**

| Target power | Target current | B7 cap |
|---|---|---|
| 50 kW | ~130 A (at 385 V) | `0x82` |
| 40 kW | ~104 A (at 385 V) | `0x68` |
| 25 kW | ~65 A (at 385 V) | `0x41` |

**Open questions:**
- Does the CCM verify B7 against another field (e.g. 0x597 B1 or 0x599) and reject implausible values?
- Does a checksum/CRC cover B7? (Likely not — no obvious CRC byte in 0x5D7 from the data.)
- Does the modification need to start before the taper begins (at the constant-current phase), or only once the taper starts?

---

## Open questions

- Which exact message and byte carries the charge current limit to the CCM?
- Is the setpoint in units of Amps, Watts, or a percentage?
- Does the BMS send the setpoint periodically or only on change?
- Is there a separate "terminate charging" flag, or does setting current to
  zero serve as termination?
- What does 0x57F contain? (required for SLAC, content unknown)
