# Bench Testing — CCM with Simulated Vehicle and Charger

## Goal

Find out, without car and real charger, which CAN message controls the charge
current requested by the CCM. On the bench, the hypothesis from
[analysislog.md](analysislog.md) (0x5D7 B7 = current setpoint) becomes a
controlled experiment: change the candidate signal on CAN, and observe the
reaction in the V2G messages which the CCM sends to the charger.

## Setup

```
  PC: vehicle simulation             MITM board            CCM              RPi2023: charger simulation
  (Python, PEAK PCAN-USB Pro) --CAN1-- (optional) --CAN2-- (bench) --CP/PE-- (pyPLC EVSE mode, PLC modem)
   |                                                                                     |
   +------------------------------------------ SSH ------------------------------------+
```

- **CCM:** bench unit, CAN on CAN2 of the MITM board (or directly on the PCAN).
  Only CP and PE are connected. The CCM has no PP input. It does only the high
  level communication (SLAC, SDP, TCP, DIN/ISO); it does not care about the 5%
  PWM and the CP states B/C.
- **Charger simulation:** pyPLC in EVSE mode on the RPi2023, modem connected to
  CP and PE of the CCM. Controlled remotely via SSH. pyPLC logs the decoded V2G
  messages, including the EVTargetCurrent which the CCM requests.
- **Vehicle simulation:** Python script on the PC, sending the vehicle side CAN
  messages via the PEAK PCAN-USB Pro.
- **MITM board:** can be inserted between PCAN and CCM, to test the firmware
  modification in the loop.
  Its CAN1 termination is removed (for the car). On the bench, a separate
  termination resistor is on the line between PCAN and board CAN1.

## What the bench can answer

- **Setpoint or echo?** Vary 0x5D7 B7 during CurrentDemand. If the
  EVTargetCurrent in the CurrentDemandReq follows, causality is proven.
  - First hint already from the bench: the CCM alone sends only 0x5E5, 0x5E6,
    0x5E8 and 0x5E9, never 0x5D7. So 0x5D7 comes from the vehicle side.
- **Plan B:** the same experiment with 0x597 B1.
- **Robustness of the CCM:** does it tolerate steps in the setpoint, frozen
  toggle bits or missing messages? (Observe SessionStop or errors in pyPLC.)
- **MITM in the loop:** implement the cap in the firmware and check that the
  capped value appears in the V2G messages (rehearsal of steps 303 and 310).

## What the bench can NOT answer

- The reaction of the real vehicle (VCU/BMS) to a capped current, e.g. a
  plausibility check of requested current against measured IBatt (0x595).
- The real current flow. The field test (steps 309 and 310) stays necessary,
  but with lower risk and fewer attempts.

## Known starting point

From the CCM investigations
(`C:\UwesTechnik\Ioniq28Investigations\CCM_ChargeControlModule_PLC_CCS\readme.md`):

- Only three messages are required to make the CCM start the PLC communication:
  0x57F, 0x597 (toggle bit), 0x599 (CCS trigger bit in byte 7 bit 0, SOC in byte 2).
- Replaying these three messages from the HPC trace against a physical CCM, the
  CCM performs SLAC and SDP, and comes until PowerDeliveryReq (with 34% SOC).
  Afterwards it sends a SessionStopReq. Presumably the handshake flags in 0x5D7
  (PreChCnf, ContactorsClosed) are missing.

## Vehicle simulation

A plain replay is not sufficient, because the session is a handshake. The
simulation ([tools/vehiclesim.py](../tools/vehiclesim.py)) is a state machine,
which reads the CCM progress and answers like the car. Sequence and content
are taken from the session start in the HPC trace (t = 2710..2736 s):
`C:\UwesTechnik\IoniqMotorCAN\IoniqMotorCAN\Traces\ioniq2018_motorCAN_500k_CCS_charging_on_HPC\ioniq2018_motorCAN_500k_HPC.asc`

**CCM signals (CCM → vehicle):**

| Signal | Meaning |
|---|---|
| 0x5E9 B0 (bit 0 = alive toggle) | progress: `02` charge parameters known, `06` cable check, `0E` precharge, `1E` power delivery, `3E` current demand; bit 6 (`4x`) = stop/error |
| 0x5E6 B0 bit 3 | cable check running (`09`) |
| 0x5E6 B1 bit 2 | cable check done (`30` → `34`) |
| 0x5E6 B0 bit 7 | current demand (`81`) |
| 0x5E8 B6 | EVSEStatusCode from the charger (`01` Ready, `04` IsolationMonitoringActive, `00` NotReady); `D0`/`D1` before an abort |
| 0x5E5 B0..B1 / B2..B3 / B4..B5 / B6..B7 | EVSE max voltage / max current / present voltage / present current (all 0.1, LE16) |

**Vehicle answers (vehicle → CCM):**

| Trigger | Vehicle answer |
|---|---|
| start | 0x5D7 B1 bit 6 `CCS_firstTrigger`, bit 3 `CcsStart`; 0x599 B7 bit 0 CCS trigger |
| progress `02` | 0x5D7 B0 bit 0 `ConnectorLockConfirm`, 0x599 B0 = `09` |
| progress `06` | (inlet voltage 0x542 B6..B7 (0.1 V) rises to 500 V — not evaluated by the CCM) |
| 0x5E6 B1 bit 2 (cable check done) | (inlet voltage ramps down below 6 V), target current 1 A and target voltage = battery voltage (346.5 V) in **0x59C** |
| progress `0E` | (inlet voltage follows the EVSE present voltage); `PreChCnf` (0x5D7 B1 bit 1) and 0x599 B1 = `10` when the EVSE present voltage (0x5E5 B4..B5) reaches the battery voltage |
| progress `1E` | `ContactorsClosed` (0x5D7 B1 bit 4), 0x597 B7 = `02`, 0x599 B7 = `03` |
| progress `3E` | target current profile in 0x59C B4..B5 (and the mirror in 0x5D7), target voltage 412.8 V |

Additionally the simulation sends 0x5A3 (static, needed for
ChargeParameterDiscovery). No other messages are needed by the CCM (see log
"Minimum set of vehicle messages").

## Steps

- [x] Step B1: Wake the CCM with the minimal set (0x57F, 0x597, 0x599) from the
  vehicle simulation. Check SLAC and SDP succeed in pyPLC. (Reproduces the
  known starting point with the new setup.) See log 2026-10-26.
- [x] Step B2: Implement the vehicle state machine, until the CCM reaches
  CurrentDemand. Works, see log.
- [x] Step B3: Find the signal which controls EVTargetCurrent. Result:
  **0x59C B4..B5**, not 0x5D7. See log.
- [ ] Step B4: (obsolete, 0x59C found) Same sweep with 0x597 B1.
- [ ] Step B5: Robustness tests (frozen toggle, missing messages).
- [x] Step B6: Insert the MITM board, implement the cap in firmware, verify
  the capped value in the V2G messages. Works (readme step 304), see log.
- [x] Step B7: Pedal control end-to-end: the vehicle simulation sends pedal
  taps (0x200 / 0x394), the limit in EVTargetCurrent follows (readme step 306).

## Tools

- [tools/canmonitor.py](../tools/canmonitor.py): observe the CAN traffic with
  the PCAN adapter (summary per ID, or every frame with `--raw`).
- [tools/vehiclesim.py](../tools/vehiclesim.py): vehicle simulation (state
  machine, see above). Logs every content change of 0x5E5..0x5E9 (alive bit
  of 0x5E9 ignored, 0x5E5 max once per second). Options: `--current` current
  profile (`t:A,...`, t = s after start of current demand), `--current-59c`
  separate profile for 0x59C, `--ramp` A per 100 ms (0 = steps), `--uccs`
  inlet voltage variant, `--omit` IDs which are not sent at all, `--add`
  additional static messages.
  This is the investigation tool. A clean version for users, which sends
  exactly the needed messages plus 0x59E and has only the options duration
  and current profile, is in Ioniq28Investigations/CCM_ChargeControlModule_PLC_CCS/vehiclesim.py.
- [tools/benchsession.py](../tools/benchsession.py): one session with CCM
  power cycle (iobox DOUT0 on RPi2023, `0` = on, `1` = off; the same supply
  powers the MITM board), pyPLC restart with a new random key (stop, 1 s,
  start, 5 s for the re-key), CCM power on, vehicle simulation, and the
  session progress from the pyPLC log, plus every change of EVTargetCurrent.
  Example: `python tools/benchsession.py -d 90 --current 0:60,25:30,45:80`
  - The MITM board is side-powered by the ST-Link (3.3 V rails connected, see
    readme flash notes), so the iobox power cycle resets only the CCM, not
    the board. This is fine: the board detects the new session from the CCM
    progress. (Earlier, for some runs, the board was power cycled too.)
- pyPLC on RPi2023 (`pi@192.168.2.118`), started by the supervisor of the
  qca7000-transparency bench:
  - mode: `ssh pi@192.168.2.118 'echo infiniteSessionRandomKey > ~/bench/pyplc.mode'`
    (one pyPLC run, which serves session after session)
  - status: `/tmp/pyplc/ctl.log`, full log of the current run: `/tmp/pyplc/current.log`
  - progress of a session: `grep '"msgName"' /tmp/pyplc/current.log`
  - benchsession.py sets `fastCableCheck=0`: with the fast cable check, pyPLC
    reports "Finished" on the first request, before the inlet voltage could
    be simulated.
  - OpenV2Gx on RPi2023 is patched (uncommitted, `git diff` in
    `~/myprogs/OpenV2Gx`, old binary `Release/OpenV2G.exe.before-cablecheck-status`):
    CableCheckRes reports EVSE_IsolationMonitoringActive while ongoing, and
    EVSE_Ready + isolation Valid when finished, like a real charger. Without
    this, the CCM does not start the precharge.

## Log

### 2026-10-26: Step B1, minimal message set

Setup: PCAN (vehicle simulation) on CAN1 → MITM board (unmodified routing) →
CAN2 → CCM. pyPLC in `infiniteSessionRandomKey`.

Message content from the HPC trace at t=2760 s (charging, SOC 34.5%), all 100 ms:

| ID | Data |
|---|---|
| 0x57F | `94 76 92 00 07 20 00 05` |
| 0x597 | `05`/`85` (toggle) `BE 00 00 00 00 00 02` |
| 0x599 | `09 10 45 D0 07 20 10 03` (B2 = SOC 34.5%, B7 bit 0 = CCS trigger) |

**Run 1** (fresh CCM): session progress in pyPLC:
SLAC → SDP → supportedAppProtocolReq → SessionSetupReq → ServiceDiscoveryReq →
ServicePaymentSelectionReq → ContractAuthenticationReq → PowerDeliveryReq
(`ReadyToChargeState = false`) → SessionStopReq.

- Same result as the earlier replay: the known starting point is reproduced.
- New detail: there is **no ChargeParameterDiscoveryReq**. The CCM aborts
  directly after ContractAuthentication (~2.5 s), with a PowerDeliveryReq
  ReadyToCharge=false. Presumably it waits for a vehicle confirmation which is
  missing (0x5D7 flags, e.g. ConnectorLockConfirm).

CCM messages during run 1 (t relative to start of the simulation):

| t (s) | ID | Data | Remark |
|---|---|---|---|
| 0 | 0x5E6 | `00 F0 00 00 00 00 00 00` | idle |
| 0 | 0x5E9 | `00`/`01` ... | B0 bit 0 alive toggle |
| 9.4 | 0x5E6 | `01 F0 ...` | |
| 10.3 | 0x5E6 | `01 30 ...` | |
| 13.1 | 0x5E8 | `... D0 00` (B6) | |
| 13.4 | 0x5E6 | `01 34 ...` | |
| 13.4 | 0x5E8 | `... D1 00` (B6) | |
| 13.4 | 0x5E9 | `40`/`41` ... | B0 bit 6 set |

(The rest of run 1 was not captured on the CAN side.)

**Run 2 and 3:** no new session. The CCM stays in `5E6 = 03 34`,
`5E8 B6 = D1`, `5E9 = 40/41`, also when the CCS trigger (0x599 B7) is off for
3 s and then on again. After a SessionStop, the CCM obviously needs something
else to start a new session (power cycle, bus sleep, or another signal).

Observation (user, on the EVSE modem): the CCM stayed associated all the time.
After a manual power cycle it associated again without SLAC, because its modem
stored the network key of the last join.

### 2026-10-26: Clean sessions with power cycle and new key

Tool [benchsession.py](../tools/benchsession.py): CCM + MITM board power off
(iobox DOUT0 = 1), pyPLC restarted with a new random key, CCM power on
(DOUT0 = 0), vehicle simulation 45 s.

| pyPLC run | Condition | SLAC_PARAM.REQ | MNBC_SOUND | SLAC_MATCH | CCM (0x5E6 B0, 0x5E8 B6..B7) |
|---|---|---|---|---|---|
| 1 | CCM fresh (B1 run 1 above) | 2 | 10 | 1 | `01` ..., session until SessionStop |
| 2 | power cycle, new key, CCS trigger after 3 s | 0 | 0 | 0 | `02`, `10 01` after 13.6 s |
| 3 | power cycle, new key, CCS trigger at once | 17 | 0 | 0 | `02`, `10 01` after 10.6 s |
| 4 | same as 3, with tcpdump on RPi2023 eth0 | 13 | 0 | 0 | `02`, `10 01` after 10.6 s |

tcpdump (ethertype 0x88E1) in run 4: the CCM modem (`e0:0e:e1:ff:d3:e2`)
broadcasts CM_SLAC_PARAM.REQ (0x6064) 13 times, pyPLC answers each one with a
unicast CM_SLAC_PARAM.CNF (0x6065). The CCM never proceeds to the sounding, and
gives up after ~10 s (`5E6 B0 = 02` presumably = SLAC failed).

Wire timing in run 4: the 13 REQs came as bursts, ~2 ms apart (6 at once, 7
more 25 ms later), instead of the normal ~80 ms spacing of run 1. pyPLC answered
all within ~100 ms, but the CCM did not proceed.

**Cause: wrong bench sequence.** The CCM was switched on directly after
pyPLC's SET_KEY.CNF, while the EVSE modem was not yet ready after the re-key.
(A stored network key in the CCM modem is NOT a problem: SLAC is exactly the
procedure in which the CCM gets the new key, in the SLAC_MATCH.CNF.)

Correct sequence (now in benchsession.py): CCM off → stop pyPLC → wait 1 s →
start pyPLC → wait 5 s until the modem is re-keyed → CCM on → vehicle CAN.

| pyPLC run | Condition | Session progress |
|---|---|---|
| 6 | correct sequence, CCS trigger at once | 2x SLAC_PARAM → SLAC_MATCH → … → ContractAuthenticationReq → PowerDeliveryReq → SessionStopReq |
| 7 | same, repeated | same |

CCM messages in run 6 and 7 (t after CCM power on), reproducible within ~0.1 s:

| t (s) | 0x5E6 | other |
|---|---|---|
| 0 | `00 F0` | |
| 8.9 | `01 F0` | |
| 9.8 | `01 30` | |
| 12.7 | | 0x5E8 B6 = `D0` |
| 13.0 | `01 34` | 0x5E8 B6 = `D1`, 0x5E9 B0 = `41` |
| 17.1 | `03 34` | (after SessionStop) |

With this sequence, sessions can be repeated as often as needed. Step B1 is
complete; next is B2 (0x5D7 handshake to get past ContractAuthentication).

### 2026-10-26: Step B2, complete session up to CurrentDemand — WORKS

(pyPLC run 18) SLAC → … → ChargeParameterDiscoveryReq → 26x CableCheckReq →
2x PreChargeReq → PowerDeliveryReq → **529x CurrentDemandReq** (whole 90 s
simulation) → PowerDeliveryReq → SessionStopReq. EVTargetCurrent follows the
simulated vehicle current (ramp 1 A / 100 ms up to 50 A).

The way there, each step found by comparing bench and car trace:

1. **0x5D7 handshake** (ConnectorLockConfirm etc.) added: no change, the CCM
   still aborted after ContractAuthenticationReq (5E8 B6 = `D0`/`D1`).
2. **Background messages** (snapshot of all vehicle 0x5xx IDs): the CCM sends
   ChargeParameterDiscoveryReq and CableCheckReq. Then it aborts 2.5 s after
   the first CableCheckReq.
3. **Inlet voltage 0x542** simulated, and **pyPLC fastCableCheck=0**: 26x
   CableCheckReq, then abort 2.5 s after "Finished". (Later shown: the inlet
   voltage is not evaluated by the CCM, see log "inlet voltage".)
4. **pyPLC CableCheckRes status** (OpenV2Gx patch, see Tools): 5E8 B6 now
   `04` during cable check, `01` after, like in the car. The CCM reports
   cable check done (5E6 = `01 34`) but still sends no PreChargeReq.
5. **0x59C** (found by searching all IDs which change in the car between end
   of cable check and start of precharge): B2..B3 target voltage, B4..B5
   target current. With 346.5 V / 1 A in 0x59C, the CCM starts the precharge,
   and the session runs through to CurrentDemand.

### 2026-10-26: Step B3, which signal controls EVTargetCurrent

Two candidates carry the same current in the car: 0x5D7 B2..B3 (0.1 A) / B7
(1 A), and 0x59C B4..B5 (0.1 A). The bench varies them independently
(times = s after start of current demand):

| Experiment | 0x5D7 | 0x59C | EVTargetCurrent (pyPLC) |
|---|---|---|---|
| E1 (run 19) | 60 → 30 (25 s) → 80 A (45 s) | 60 A | **60 A constant** |
| E2 (run 20) | 60 A | 60 → 30 (25 s) → 80 A (45 s) | **60 → 30 → 80 A**, follows 0x59C |
| E3 (run 21) | steps without ramp: 20 → 80 (10 s) → 10 A (25 s) | same | **20 → 80 → 10 A**, each step in the next CurrentDemandReq |

**Result:**
- **0x59C B4..B5 (LE16, 0.1 A) controls the EVTargetCurrent.** 0x5D7 has no
  influence (it is only a mirror of the same value in the car).
- The CCM forwards the value 1:1, without own ramp or rate limit. (The ramp
  seen in the car comes from the vehicle side.)
- 0x59C has no visible counter or checksum (B0..B1 `BC A6` and B6..B7 `60 6D`
  static), the CCM accepts modified values.
- The CurrentDemandReq also contains EVMaximumCurrentLimit = 200.0 A
  (presumably 0x599 B3..B4 = 0x07D0) and EVMaximumPowerLimit = 98 kW
  (0x2648 = 9800, in 0x594 / 0x59E). These were not modified.

### 2026-10-26: Does the CCM evaluate the inlet voltage 0x542?

The simulation reproduced the inlet voltage like in the car (500 V during the
cable check, below 6 V afterwards, then following the EVSE voltage). It was
added before the real blockers (EVSEStatusCode, 0x59C) were found, so it was
never shown to be necessary. Test with `--uccs` (MITM board reflashed with
ST-LINK_CLI, pedal firmware, limit 100 A, not active at 50 A):

| pyPLC run | 0x542 inlet voltage | Result |
|---|---|---|
| 22 | `sim`: like the car | complete session, 217x CurrentDemandReq, 50 A |
| 23 | `const`: 4.9 V during the whole session | complete session, 223x CurrentDemandReq, 50 A |
| 24 | `high`: stays 500 V after the cable check until precharge | complete session, 223x CurrentDemandReq, 50 A |

In all runs the CCM went from "cable check done" to PreCharge within ~0.3 s.
**Result: the CCM does not evaluate the inlet voltage.** The simulation keeps
the car-like behavior as default.

### 2026-10-26: Minimum set of vehicle messages

Question: which of the messages are really needed for a complete session
(CurrentDemand)? Fast test (`benchsession.py --fast`, ~30 s per run: the
simulation ends 2 s after the CCM reports CurrentDemand, or at once on an
abort; criterion is the CCM progress 0x5E9 on CAN). Messages are removed
completely with `--omit` or by a reduced background file. (The background
files, snapshots of the car trace, were deleted after this investigation:
the CCM needs none of them. 0x5A3 is now sent by the simulation itself.)

1. Background with only the 5 messages which carry charge parameter values
   (0x594, 0x595, 0x59E, 0x5A3, 0x5D8, found by searching the values of the
   ChargeParameterDiscoveryReq in the snapshot): **OK**. Without any
   background: NOK (stop after ContractAuthenticationReq).
2. Leave-one-out, starting from this set:

| Omitted | Result |
|---|---|
| 0x594 | OK |
| 0x595 | OK |
| 0x59E | OK |
| **0x5A3** | **NOK**, abort after ContractAuthenticationReq |
| 0x5D8 | OK |
| **0x57F** | **NOK**, no SLAC |
| **0x597** | **NOK**, no SLAC |
| **0x599** | **NOK**, no SLAC |
| 0x5D7 | OK |
| **0x59C** | **NOK**, abort after CableCheck |
| 0x542 | OK |

3. Combined minimal set, only **0x57F, 0x597, 0x599, 0x59C, 0x5A3**: OK (fast
   test), and final confirmation over 75 s with a current profile: 426x
   CurrentDemandReq, EVTargetCurrent follows 0x59C (60 → 30 → 80 A).

**Result: the CCM needs only 5 vehicle messages:**

| ID | Content used by the simulation |
|---|---|
| 0x57F | `94 76 92 00 07 20 00 05` (static) |
| 0x597 | B0 bit 7 toggle, B1 `C3`, B7 `00` → `02` at power delivery |
| 0x599 | B0 `08` → `09`, B1 `00` → `10`, B2 SOC, B7 bit 0 CCS trigger (`01` → `03`) |
| 0x59C | B2..B3 target voltage, B4..B5 target current (see above) |
| 0x5A3 | `63 30 48 00 00 00 00 00` (static; B1..B2 = 18480 = EVEnergyRequest in Wh?) |

Not needed: 0x5D7 (none of the handshake flags ConnectorLockConfirm, PreChCnf,
ContactorsClosed), 0x542 (inlet voltage), and all other messages of the car.
Not yet tested: which bytes inside the 5 messages are needed, e.g. whether the
dynamic changes of 0x597 / 0x599 (B0, B1, B7) are required.

### 2026-10-26: Where does the remaining time come from?

The DIN CurrentDemandReq has the optional fields RemainingTimeToFullSoC and
RemainingTimeToBulkSoC. The pyPLC decoder (OpenV2Gx) did not print them; it was
extended (DIN and ISO1, together with BulkChargingComplete; pulled upstream
1ecbedd first, the change is to be committed by uhi22).

With the minimal simulation, the CCM sends both fields (unit s), but with
value 0, although 0x5D7 B4..B5 = 840 s is sent. Fast tests with additional
messages, with distinctive values (`benchsession.py --fast --add ID:DATA`):

| Added | RemainingTimeToFullSoC | RemainingTimeToBulkSoC |
|---|---|---|
| nothing | 0 | 0 |
| 0x59E `57 04 09 03 89 0D 48 26` (B0..B1 = 1111, B2..B3 = 777) | **1111 s** | **777 s** |
| 0x596 `00 82 00 00 21 00 1A 1C` (B4 = 33, DBC `remainingChTDC` in min) | 0 | 0 |

**Result:** 0x59E B0..B1 (LE16, s) = RemainingTimeToFullSoC, B2..B3 (LE16, s) =
RemainingTimeToBulkSoC, forwarded 1:1 by the CCM. 0x5D7 B4..B5 ("remainingTimeBulk")
and 0x596 have no influence; in the car, 0x5D7 B4..B5 carries the same value as
0x59E B2..B3 (840 s at the session start), so it is again only a mirror.
0x59E is not needed for the session itself (see minimum set); without it, the
remaining times are 0.

Additionally found: **0x59E B6..B7 (LE16) = EVMaximumPowerLimit** (car 9800 =
98 kW; verified with 4660 → 4660 in ChargeParameterDiscoveryReq and
CurrentDemandReq). Without 0x59E, the CCM announces EVMaximumPowerLimit = 0
(pyPLC ignores it, a real charger may not). So for a real charger, 0x59E is
needed. It is also a second possible lever for the power reduction (not tested
whether a charger honors it).

### 2026-10-26: Step B6, MITM cap — firmware built, bench power lost

Firmware change ([canhandler.c](../src/canhandler.c)): in the CAN1 → CAN2
path, 0x59C B4..B5 is limited to `CHARGE_CURRENT_LIMIT_DECIAMPS` (400 = 40 A
for the bench test). Counter `nTargetCurrentLimited` printed on the serial
line ("target current limited: n"). Built without warnings, NOT flashed yet.

Around 16:36, directly after experiment E3, the CCM and the MITM board lost
their power: no serial output, no CAN traffic, ST-Link reports "No target
connected", independent of iobox DOUT0 (0 and 1 tried; the iobox confirms
the commands). Needs a check on the bench (12 V supply / wiring).

Update: the board had power (ST-Link target voltage 3.3 V), the GD32 was hung.
Recovery attempts:
- A power cycle (DOUT0 off 5 s, on) brings the old firmware back reliably.
- With the firmware running, SWD connect fails ("Unable to connect to the MCU
  device"). Connect under reset (`UR`) works sometimes, but programming then
  fails ("Internal command error"), and afterwards the GD32 is hung again.
- OpenOCD (ST-Link, `set CPUTAPID 0`) reads DPIDR 0x1ba01477 and detects the
  Cortex-M3, but every memory access fails ("Fail reading CTRL/STAT register").
  One attempt erased the flash before the write failed: **the board is blank
  now** and does not route CAN until it is flashed again.
- Since the side-power path of the ST-Link was removed, SWD is unreliable
  (in the morning, flashing worked at the first attempt). Suspected: ground
  or signal integrity of the SWD wiring.

Recovered: on the blank chip (no firmware running), ST-LINK_CLI with normal
connect (`-c SWD FREQ=480 -P ... -V -Rst`) programmed and verified at the
first attempt. (A following OpenOCD attempt erased it again; a second
ST-LINK_CLI run fixed it.) The board now runs the pedal firmware (limit 100 A).
Lesson: do not use OpenOCD with this ST-Link/GD32; its failed write erases the flash.

To do (cap test):
1. Flash: `ST-LINK_CLI.exe -c SWD -P chargepower-reducer.hex -V -Rst`
2. `python tools/benchsession.py -d 90 --current 0:150,30:60`
   → expected: EVTargetCurrent 100 A (the default limit of the pedal control,
   see [pedal-control.md](pedal-control.md)), then 60 A; serial "limited"
   counting while 150 A is requested.

(Firmware change since then: the fixed 40 A cap was replaced by the pedal
controlled limit, default 100 A.)

### 2026-10-26: Step B6 / readme step 304, cap test — WORKS

MITM board with the pedal firmware (limit 100 A by default, no pedal messages
on the bench: pedals "missing", limit unchanged).
`python tools/benchsession.py -d 75 --current 0:150,30:60`:

| Vehicle target current (0x59C) | EVTargetCurrent (pyPLC) |
|---|---|
| ramp to 150 A | 100 A (capped, from 10 s after start of CurrentDemand) |
| 60 A (after 30 s) | 60 A |

Complete session, 423x CurrentDemandReq. Serial line afterwards:
`limit: 100A  limited: 250  accel: 0 missing taps 0  brake: missing taps 0  sessions: 1`
(250 capped frames ≈ 25 s × 10/s above 100 A; one session start detected).

### 2026-10-26: CAN1 termination of the MITM board removed

The 120 ohm termination on the CAN1 side of the board was removed (preparation for
the car, see readme "Bus termination"). Fast test afterwards: complete session up to
CurrentDemand, no receive drops on the board. On the bench, the line PCAN ↔ board CAN1
is terminated by a separate bench termination resistor.

### 2026-10-26: Step B7 / readme step 306, pedal control end-to-end — WORKS

vehiclesim.py got `--taps t:kind,...` (t = s after start of current demand; brake /
accel = 0.3 s press, brakelong / accellong = 2 s). It sends 0x200 (B4 accelerator,
0x80 = 50 % during a tap) and 0x394 (B6 bit 7 brake) always, released outside the taps.

`python tools/benchsession.py -d 62 --current 0:150 --taps 10:brake,15:brake,25:accel,35:brakelong`
(the vehicle requests 150 A all the time, so EVTargetCurrent shows the limit):

| t in CurrentDemand | Action | EVTargetCurrent (pyPLC) |
|---|---|---|
| until 10 s | — | 100 A (default limit) |
| 10 s | brake tap | 90 A (at 10.7 s) |
| 15 s | brake tap | 80 A (at 15.7 s) |
| 25 s | accelerator tap | 90 A (at 25.8 s) |
| 35 s | brake pressed 2 s | unchanged 90 A (long press ignored) |

Serial line afterwards: `limit: 90A  limited: 358  accel: 0 missing taps 1  brake: missing taps 2  sessions: 1`
("missing" because the simulation had ended: the signal timeout works too).

### 2026-10-26: Pedal control at the limits — WORKS

1. Raise above the request of the car, and the 200 A clamp:
   `--current 0:150,40:30`, 11 accelerator taps (5..15 s), 17 brake taps (45..61 s),
   1 accelerator tap (64 s). EVTargetCurrent follows the request of the car (ramp
   to 150 A, then down to 30 A), never more; `limited: 0`. Final limit 40 A =
   100 + 110 → clamped 200, − 170, + 10 (without the clamp it would be 50 A).
2. The 30 A clamp: `--current 0:150`, 12 brake taps (5..16 s). The limit goes down
   to 30 A after 7 taps and stays there; EVTargetCurrent 30 A while the car
   requests 150 A (`limit: 30A  limited: 181  brake taps 12`).

The firmware only lowers 0x59C; a limit above the request of the car has no
effect.

### 2026-10-26: Pedal taps relative to the delivered current; SWD bug found; board not flashed

Problem: the driver sees only the delivered power. With the limit at 200 A and the car
requesting 175 A, a brake tap (limit 190 A) has no visible effect; with a small charger
(150 A) even more taps are invisible. New logic in pedalhandler.c: a tap sets the limit to
the **delivered current** (EVSE present current, from the CCM in 0x5E5 B6..B7) ± 10 A,
clamped to 30..200 A. Taps are evaluated only during CurrentDemand with a recent 0x5E5;
otherwise they are counted as "ignored taps" (serial line). PC self-test extended: 17 checks
passed (car as bottleneck 175 → 165 A, small charger 150 → 140 A, taps before CurrentDemand
ignored, 0x5E5 timeout).

pyPLC (experimental, not committed, on RPi2023): if `~/bench/evse_max_current` exists, the
simulated charger follows the EVTargetCurrent with a short lag, limited to this maximum
(without the file: unchanged constant 10 A). Backup: `/tmp/hardwareInterface.py.before-evse-current`.
Test with 150 A max and the car requesting 175 A: 0x5E5 present current rises to 150 A and stays.

SWD bug: canhandler.c did `AFIO_MAPR |= AFIO_MAPR_CAN2_REMAP`. The SWJ_CFG bits in
AFIO_MAPR are write-only (read undefined); the read-modify-write may disable SWD on the GD32,
which explains why flashing worked only on a blank chip. Fixed with
`gpio_primary_remap(AFIO_MAPR_SWJ_CFG_JTAG_OFF_SW_ON, AFIO_MAPR_CAN2_REMAP)`.

Flashing the new firmware: ST-LINK_CLI under reset, attempt 1 programmed and verified OK, but
the board did not answer directly afterwards (probably still held in reset); a script retried
after a power cycle, attempt 2 failed ("Programming error @ 0x08000000"). Since then the board
does not boot, and the ST-Link reports "No target connected" also after power cycles of the
board. Needs manual action (e.g. re-plug the ST-Link USB). Lesson: after a successful
programming, power cycle and check before retrying.

### 2026-10-26: SWD root cause found, new pedal logic verified on the board

Root cause of the unreliable SWD (found by uhi22): the "3.3V" pin of the ST-Link V2 clone
is a supply output at only 2.8 V; the 3.3 V rail of the board is load dependent (~3.4 V
while the controller does nothing, 3.30 V while it runs). The level difference breaks the
SWD communication. With 3.3 V from a lab supply on either side, flashing worked perfectly.
Solution: ST-Link "3.3V" and board 3.3 V connected. Consequence: the iobox power cycle no
longer resets the board.

Then: flashed the blank board with ST-LINK_CLI (normal connect), and 3 more times while
the firmware was running (with the AFIO_MAPR / SWJ_CFG fix): all verified OK, firmware
starts each time.

New pedal logic on the board, pyPLC charger maximum 150 A (`~/bench/evse_max_current`),
car requests 175 A, `--taps 10:accel,15:accel,20:accel,25:accel,30:accel,35:accel,40:brake,45:brake`:
delivered current (0x5E5 B6..B7) 100 → 108 → 118 → 128 → 138 → 148 → 150 (charger
maximum; limit about 158 A) → **brake: 140 A at once** → brake: 130 A. Serial line:
`limit: 130A  present: 130A  limited: 435  … taps 6 … taps 2  ignored taps: 0  sessions: 1`.
(The steps are relative to the delivered current at the tap, which lags the target a bit
in the pyPLC simulation, so 108 instead of 110.)

### 2026-10-26: SOC for the charger

vehiclesim.py / benchsession.py got `--soc` (0x599 B2, 0.5 %/LSB, default 34).
`--fast --soc 61.5` (0x599 B2 = 123): the CCM sends EVRESSSOC = 61 in all requests
(68x). So 0x599 B2 is the SOC which the charger gets; the CCM drops the half percent.

### 2026-10-26: Serial filter, load test, TX backpressure

Firmware: the serial output stores and prints only 0x59C, 0x599, 0x200, 0x394 (vehicle) and
0x5E5, 0x5E9 (CCM); tables 8 entries instead of 128. Serial task 0.4 ms (no vehicle traffic)
/ 0.75 ms (full traffic), before ~5 ms.

Load test: replay of the HPC trace on the PCAN (CAN1) with original timing, all 88 vehicle IDs
(without the CCM IDs 0x5E5..0x5E9), ~2 210 frames/s:
- before: no RX drops, but 36..70 TX drops on CAN2 (to the CCM) per 30 s: all 3 TX mailboxes
  busy for a moment, the frame was discarded.
- fix: if no TX mailbox of the destination CAN is free, the frame stays in the RX ring and is
  routed in the next main loop. Only if the ring is nearly full (destination not acknowledged),
  it is processed anyway and counted as TX dropped.
- after: 2 × 30 s (start of current demand, taper), 132 726 frames: **0 RX drops, 0 TX drops on CAN2**.

Side effect on the bench: while CAN1 is not acknowledged (no PCAN application open), the RX ring
of CAN2 fills with CCM frames, which arrive late when the PCAN is opened again. vehiclesim.py
therefore ignores the CCM progress until it has seen the CCM idle (progress 0) in this run.
(In the car, CAN1 is always acknowledged.)

Observation: taps during the current ramp-up act on the momentary delivered current (brake tap
at 5 s after start of CurrentDemand: limit ~37 A).
