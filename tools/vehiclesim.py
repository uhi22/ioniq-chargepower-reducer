"""Vehicle simulation for bench testing the CCM, via a PEAK PCAN adapter.

Sends the vehicle side messages 0x57F, 0x597, 0x599, 0x5D7 (all 100 ms), and
answers the progress of the CCM (0x5E9 B0) with the handshake flags, like the
car does. Logs every content change of the CCM messages 0x5E5..0x5E9.

Sequence and message content are taken from the session start in the HPC
trace (t=2710..2736 s), ioniq2018_motorCAN_500k_HPC.asc. See doc/bench-testing.md.

CCM progress (0x5E9 B0, bit 0 is an alive toggle):
  0x02 charge parameters known   -> ConnectorLockConfirm (5D7 B0 bit 0), 599 B0 bit 0
  0x06 cable check              -> inlet voltage (542) ramps to 500 V and holds (*)
  5E6 B1 bit 2: cable check done  -> inlet voltage ramps down below 6 V (*), then target current 1 A and
                                    target voltage = battery voltage in 59C (and 5D7). The CCM waits
                                    for this before the precharge.
  0x0E precharge                 -> inlet voltage follows the EVSE voltage (5E5 B4..B5) (*),
                                    PreChCnf (5D7 B1 bit 1) and 599 B1 = 0x10 when the EVSE voltage
                                    reaches the battery voltage
  0x1E power delivery            -> ContactorsClosed (5D7 B1 bit 4), 597 B7 = 2, 599 B7 = 3
  0x3E current demand            -> target current (59C B4..B5) ramps to the current profile (1 A per 100 ms).
                                    The CCM forwards 59C B4..B5 1:1 as EVTargetCurrent to the charger.

5D7: B0 bit 0 ConnectorLockConfirm, B1 bit 6 CCS_firstTrigger, bit 3 CcsStart,
bit 1 PreChCnf, bit 4 ContactorsClosed, B2..B3 current 0.1 A (LE16), B4..B5
0x0348 (remaining time?), B7 current 1 A. (The current in 5D7 is only a mirror,
the CCM does not use it.)
542: B6..B7 inlet voltage UCCS 0.1 V (LE16), measured by the vehicle.
(*) The CCM does not evaluate the inlet voltage (the same session with --uccs const and
    --uccs high). It is simulated like in the car anyway.
5A3: static 63 30 48 00 00 00 00 00 (B1..B2 = 18480, EVEnergyRequest in Wh?). Without it, the CCM
stops before ChargeParameterDiscovery.
57F, 597, 599, 59C, 5A3 are the minimal set which the CCM needs.
5D7 and 542 are sent like in the car, but the CCM does not need them (--omit 5D7,542).
59C: B2..B3 target voltage 0.1 V (battery voltage during precharge, max voltage 412.8 V during current
demand), B4..B5 target current 0.1 A (LE16). This controls the EVTargetCurrent.
5E5 (from CCM): B4..B5 EVSE present voltage 0.1 V (LE16).

Requires: pip install python-can, and the PEAK driver (PCANBasic.dll).

Usage:
  python tools/vehiclesim.py -d 90
  python tools/vehiclesim.py -d 90 --current 0:60,20:30,40:90
      current profile, seconds after start of current demand : current in A
  python tools/vehiclesim.py -d 90 --current 0:60,20:30 --current-59c 0:60
      separate profiles for 0x5D7 and 0x59C
"""
import argparse
import time

import can

CYCLE_S = 0.1
CCM_IDS = (0x5E5, 0x5E6, 0x5E8, 0x5E9)
RAMP_A_PER_CYCLE = 1.0
SOC_PERCENT = 34.0  # sent in 0x599 B2, 0.5 %/LSB
OMIT_IDS = set()   # messages which are not sent at all (to find the minimum set of messages)
EXTRA_FRAMES = {}  # additional static messages, id -> data (experiments)
UCCS_MODE = "sim"  # inlet voltage: "sim" like the car, "const" always 4.9 V, "high" stays 500 V after cable check
BATTERY_VOLTAGE = 346.5        # V, battery voltage in the car trace (0x595)
MAX_VOLTAGE = 412.8            # V, target voltage during current demand (0x59C)
UCCS_IDLE = 4.9                # V, inlet voltage without charger voltage
UCCS_CABLECHECK = 500.0        # V

# CCM progress values (0x5E9 B0 without the alive bit)
PROG_PARAMS = 0x02
PROG_CABLECHECK = 0x06
PROG_PRECHARGE = 0x0E
PROG_POWERDELIVERY = 0x1E
PROG_CURRENTDEMAND = 0x3E


def parse_profile(text):
    """'0:60,20:30' -> [(0.0, 60.0), (20.0, 30.0)], sorted by time."""
    points = []
    for item in text.split(","):
        t, i = item.split(":")
        points.append((float(t), float(i)))
    return sorted(points)


def profile_current(profile, t):
    """Target current of the profile at t seconds after start of current demand."""
    current = profile[0][1]
    for t_point, i_point in profile:
        if t >= t_point:
            current = i_point
    return current


def ramp(value, target):
    """One 100 ms step of value towards target. RAMP_A_PER_CYCLE <= 0: jump to the target."""
    if RAMP_A_PER_CYCLE <= 0:
        return target
    if value < target:
        return min(target, value + RAMP_A_PER_CYCLE)
    return max(target, value - RAMP_A_PER_CYCLE)


TAP_KINDS = {  # name -> (pedal, duration in s)
    "brake": ("brake", 0.3),
    "accel": ("accel", 0.3),
    "brakelong": ("brake", 2.0),
    "accellong": ("accel", 2.0),
}


def parse_taps(text):
    """'10:brake,25:accel' -> [(10.0, 'brake', 0.3), (25.0, 'accel', 0.3)]"""
    taps = []
    for item in (text or "").split(","):
        if item.strip():
            t, kind = item.split(":")
            pedal, duration = TAP_KINDS[kind.strip()]
            taps.append((float(t), pedal, duration))
    return taps


class Vehicle:
    """State of the simulated vehicle, reacting on the CCM progress."""

    def __init__(self, profile, log, cablecheck_hold=1.0, cablecheck_down=1.5, profile_59c=None, taps=None):
        self.profile = profile
        self.taps = taps or []
        self.t = 0.0
        self.profile_59c = profile_59c or profile
        self.current_59c = 0.0       # A, the value sent in 59C (normally the same as in 5D7)
        self.cablecheck_hold = cablecheck_hold
        self.cablecheck_down = cablecheck_down
        self.t_cablecheck = None
        self.uccs = UCCS_IDLE        # V, inlet voltage sent in 542
        self.evse_voltage = 0.0      # V, EVSE present voltage reported by the CCM in 5E5
        self.log = log
        self.toggle = False
        self.trigger = False
        self.lock_confirm = False
        self.prech_cnf = False
        self.contactors_closed = False
        self.t_precharge = None
        self.t_cablecheck_done = None
        self.t_currentdemand = None
        self.current = 0.0   # A, the value sent in 5D7
        self.progress = 0
        self.seen_idle = False   # CCM progress 0 seen in this run (stale frames of an old session are ignored)

    def on_ccm_progress(self, progress, t):
        if not self.seen_idle:
            # frames of a previous session may still arrive at the start (e.g. buffered by the
            # MITM board while CAN1 was not acknowledged): wait until the CCM reports idle
            if progress == 0:
                self.seen_idle = True
            return
        if progress == self.progress:
            return
        self.progress = progress
        self.log(f"{t:8.3f}  CCM progress 0x{progress:02X}")
        if progress & 0x40:
            return  # stop/error, keep the flags as they are
        if progress >= PROG_PARAMS and not self.lock_confirm:
            self.lock_confirm = True
            self.log(f"{t:8.3f}  vehicle: ConnectorLockConfirm")
        if progress >= PROG_CABLECHECK and self.t_cablecheck is None:
            self.t_cablecheck = t
        if progress >= PROG_PRECHARGE and self.t_precharge is None:
            self.t_precharge = t
        if progress >= PROG_POWERDELIVERY and not self.contactors_closed:
            self.contactors_closed = True
            self.log(f"{t:8.3f}  vehicle: ContactorsClosed")
        if progress >= PROG_CURRENTDEMAND and self.t_currentdemand is None:
            self.t_currentdemand = t

    def on_cablecheck_done(self, t):
        """CCM reports the cable check done (5E6 B1 bit 2)."""
        if self.t_cablecheck_done is None:
            self.t_cablecheck_done = t
            self.log(f"{t:8.3f}  CCM: cable check done")

    def cycle(self, t):
        """Update the time dependent parts, called every 100 ms."""
        self.toggle = not self.toggle
        self.t = t
        if self.t_precharge is not None:
            self.uccs = self.evse_voltage
        elif self.t_cablecheck is not None:
            dt = t - self.t_cablecheck
            if self.cablecheck_hold >= 0:
                t_down = 0.5 + self.cablecheck_hold
            elif self.t_cablecheck_done is not None:
                t_down = self.t_cablecheck_done - self.t_cablecheck  # hold until the CCM reports the cable check done
            else:
                t_down = float("inf")
            if UCCS_MODE == "high":
                t_down = float("inf")  # experiment: the inlet voltage stays high after the cable check
            if dt < 0.5:
                self.uccs = UCCS_IDLE + (UCCS_CABLECHECK - UCCS_IDLE) * dt / 0.5
            elif dt < t_down:
                self.uccs = UCCS_CABLECHECK
            elif dt < t_down + self.cablecheck_down:
                self.uccs = UCCS_CABLECHECK - (UCCS_CABLECHECK - UCCS_IDLE) * (dt - t_down) / self.cablecheck_down
            else:
                self.uccs = UCCS_IDLE
        if UCCS_MODE == "const":
            self.uccs = UCCS_IDLE  # experiment: no inlet voltage at all
        if (self.t_cablecheck_done is not None and self.current == 0.0 and self.t_currentdemand is None
                and (self.uccs < 6.0 or UCCS_MODE != "sim")):
            # like the car: request 1 A when the inlet voltage is low after the cable check, then the CCM starts the PreCharge
            self.current = 1.0
            self.log(f"{t:8.3f}  vehicle: request 1 A")
        if (self.t_precharge is not None and not self.prech_cnf
                and self.evse_voltage >= BATTERY_VOLTAGE - 10.0):
            self.prech_cnf = True
            self.log(f"{t:8.3f}  vehicle: PreChCnf (EVSE voltage {self.evse_voltage:.1f} V)")
        if self.t_currentdemand is not None:
            self.current = ramp(self.current, profile_current(self.profile, t - self.t_currentdemand))
            self.current_59c = ramp(self.current_59c, profile_current(self.profile_59c, t - self.t_currentdemand))
        else:
            self.current_59c = self.current

    def pedal_pressed(self, pedal):
        """True while a tap of this pedal is running (taps are relative to the start of current demand)."""
        if self.t_currentdemand is None:
            return False
        dt = self.t - self.t_currentdemand
        return any(p == pedal and t_tap <= dt < t_tap + duration for t_tap, p, duration in self.taps)

    def frames(self):
        """The vehicle messages for one 100 ms cycle."""
        b1 = 0x40  # CCS_firstTrigger
        if self.trigger:
            b1 |= 0x08  # CcsStart
        if self.prech_cnf:
            b1 |= 0x02
        if self.contactors_closed:
            b1 |= 0x10
        deci_amps = int(round(self.current * 10))
        accel = 0x80 if self.pedal_pressed("accel") else 0x00
        brake = self.pedal_pressed("brake")
        deci_amps_59c = int(round(self.current_59c * 10))
        deci_volts = int(round(self.uccs * 10))
        if self.current == 0.0:
            target_voltage = 0.0
        elif self.t_currentdemand is None:
            target_voltage = BATTERY_VOLTAGE
        else:
            target_voltage = MAX_VOLTAGE
        deci_target_volts = int(round(target_voltage * 10))
        return {
            0x59C: [0xBC, 0xA6, deci_target_volts & 0xFF, deci_target_volts >> 8,
                    deci_amps_59c & 0xFF, deci_amps_59c >> 8, 0x60, 0x6D],
            # pedals (sent always, released outside of the taps):
            # 0x200 B4 accelerator 0..255 (B7 the same in %), 0x394 B6 bit 7 DriverBraking
            0x200: [0x00, 0xA8, 0x49, 0x04, accel, 0x3B, 0x3C, accel * 100 // 255],
            0x394: [0x00, 0x00, 0x00, 0x00, 0x00, 0xC4 if brake else 0x84,
                    0x97 if brake else 0x17, 0x93 if brake else 0x83],
            0x542: [0x44, 0x2E, 0x04, 0x00, 0x04, 0x01, deci_volts & 0xFF, deci_volts >> 8],
            0x57F: [0x94, 0x76, 0x92, 0x00, 0x07, 0x20, 0x00, 0x05],
            0x5A3: [0x63, 0x30, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00],  # needed for ChargeParameterDiscovery
            0x597: [0x85 if self.toggle else 0x05, 0xC3, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x02 if self.contactors_closed else 0x00],  # B0 bit 7: toggle bit
            0x599: [0x09 if self.lock_confirm else 0x08,
                    0x10 if self.prech_cnf else 0x00,
                    int(round(SOC_PERCENT * 2)) & 0xFF, 0xD0, 0x07, 0x20, 0x10,  # B2: SOC 0.5%/LSB
                    (0x03 if self.contactors_closed else 0x01) if self.trigger else 0x00],  # B7 bit 0: CCS trigger
            0x5D7: [0x03 if self.lock_confirm else 0x02, b1,
                    deci_amps & 0xFF, deci_amps >> 8, 0x48, 0x03, 0x00,
                    int(round(self.current)) & 0xFF],
        }


def parse_frames(items):
    """['59E:5704090300000000', ...] -> {0x59E: [0x57, 0x04, ...]}"""
    frames = {}
    for item in items or []:
        can_id, data = item.split(":")
        frames[int(can_id, 16)] = list(bytes.fromhex(data))
    return frames


def parse_ids(text):
    """'542,5d7' -> {0x542, 0x5D7}"""
    return {int(x, 16) for x in text.split(",") if x.strip()}


def make_logger(output):
    """-> log function which prints, and also writes to the file output (if given)."""
    logfile = open(output, "w") if output else None

    def log(line):
        print(line, flush=True)
        if logfile:
            logfile.write(line + "\n")
            logfile.flush()
    return log


def run(channel, duration, trigger_delay, log, profile=None, cablecheck_hold=-1.0,
        cablecheck_down=0.5, profile_59c=None, stop_after_cd=None, taps=None):
    """Run the vehicle simulation for duration seconds.
    stop_after_cd: fast test, end the simulation this many seconds after the CCM reached CurrentDemand,
    or at once when the CCM aborts. Returns the result: "CurrentDemand", "abort 0xNN" or "no CurrentDemand"."""
    vehicle = Vehicle(profile or [(0.0, 50.0)], log, cablecheck_hold, cablecheck_down, profile_59c, taps)
    last_ccm = {}      # id -> last data, to print only changes
    last_5e5_log = 0.0  # 0x5E5 changes all the time during current demand, log it max once per second
    with can.Bus(interface="pcan", channel=channel, bitrate=500000) as bus:
        log(f"Vehicle simulation on {channel} for {duration} s ...")
        t_start = time.monotonic()
        t_next = t_start
        result = "no CurrentDemand"
        while time.monotonic() - t_start < duration:
            now = time.monotonic()
            t = now - t_start
            if vehicle.t_currentdemand is not None:
                result = "CurrentDemand"
                if stop_after_cd is not None and t - vehicle.t_currentdemand >= stop_after_cd:
                    break
            elif vehicle.progress & 0x40:
                result = f"abort 0x{vehicle.progress:02X}"
                if stop_after_cd is not None:
                    break
            if now >= t_next:
                t_next += CYCLE_S
                vehicle.trigger = t >= trigger_delay
                vehicle.cycle(t)
                frames = dict(EXTRA_FRAMES)
                frames.update(vehicle.frames())
                for can_id, data in frames.items():
                    if can_id in OMIT_IDS:
                        continue
                    try:
                        bus.send(can.Message(arbitration_id=can_id, data=data, is_extended_id=False))
                    except can.CanError as e:
                        log(f"{t:8.3f}  send 0x{can_id:03X} failed: {e}")
            msg = bus.recv(timeout=max(0.0, t_next - time.monotonic()))
            if msg is None or msg.is_error_frame or msg.arbitration_id not in CCM_IDS:
                continue
            t = time.monotonic() - t_start
            data = bytes(msg.data)
            key = data
            if msg.arbitration_id == 0x5E9:
                key = bytes([data[0] & 0xFE]) + data[1:]  # B0 bit 0 is an alive toggle, ignore it
                vehicle.on_ccm_progress(data[0] & 0xFE, t)
            if msg.arbitration_id == 0x5E5:
                vehicle.evse_voltage = (data[4] | data[5] << 8) / 10
            if msg.arbitration_id == 0x5E6 and data[1] & 0x04 and not data[0] & 0x02:
                vehicle.on_cablecheck_done(t)
            if last_ccm.get(msg.arbitration_id) != key:
                if msg.arbitration_id == 0x5E5 and t - last_5e5_log < 1.0:
                    continue
                if msg.arbitration_id == 0x5E5:
                    last_5e5_log = t
                last_ccm[msg.arbitration_id] = key
                extra = (f"   (vehicle current 5D7 {vehicle.current:.0f} A, 59C {vehicle.current_59c:.0f} A,"
                         f" inlet {vehicle.uccs:.0f} V)"
                         if msg.arbitration_id == 0x5E5 else "")
                log(f"{t:8.3f}  0x{msg.arbitration_id:03X}  {data.hex(' ').upper()}{extra}")
    log(f"RESULT: {result}")
    return result


def main():
    parser = argparse.ArgumentParser(description="Vehicle simulation for bench testing the CCM")
    parser.add_argument("-c", "--channel", default="PCAN_USBBUS1", help="PCAN channel (default PCAN_USBBUS1)")
    parser.add_argument("-d", "--duration", type=float, default=60.0, help="run time in s (default 60)")
    parser.add_argument("-t", "--trigger-delay", type=float, default=0.0,
                        help="time in s with CCS trigger off before switching it on (default 0)")
    parser.add_argument("--current", default="0:50",
                        help="current profile t:A,t:A,... (t = s after start of current demand, default 0:50)")
    parser.add_argument("--current-59c", help="separate current profile for 0x59C (default: same as --current)")
    parser.add_argument("--ramp", type=float, default=1.0, help="current ramp in A per 100 ms, 0: steps (default 1)")
    parser.add_argument("--uccs", choices=("sim", "const", "high"), default="sim",
                        help="inlet voltage 0x542: sim (like the car), const (always 4.9 V), "
                             "high (stays 500 V after cable check until precharge)")
    parser.add_argument("--cablecheck-hold", type=float, default=-1.0,
                        help="time in s the inlet voltage stays at 500 V during cable check, "
                             "-1: until the CCM reports the cable check done (default -1)")
    parser.add_argument("--cablecheck-down", type=float, default=0.5,
                        help="time in s for the inlet voltage ramp down after cable check (default 0.5)")
    parser.add_argument("--omit", default="", help="IDs (hex, comma separated) which are not sent at all, e.g. 542,5D7")
    parser.add_argument("--soc", type=float, default=34.0, help="SOC in %% in 0x599 B2 (0.5 %% steps, default 34)")
    parser.add_argument("--taps", help="pedal taps t:kind,... (t = s after start of current demand; "
                                       "kind brake, accel (0.3 s), brakelong, accellong (2 s))")
    parser.add_argument("--add", action="append", metavar="ID:DATA",
                        help="additional static message every 100 ms, e.g. 596:0082000017001A1C (repeatable)")
    parser.add_argument("-o", "--output", help="also write the log to this file")
    args = parser.parse_args()
    global RAMP_A_PER_CYCLE, UCCS_MODE, OMIT_IDS, EXTRA_FRAMES, SOC_PERCENT
    RAMP_A_PER_CYCLE = args.ramp
    UCCS_MODE = args.uccs
    SOC_PERCENT = args.soc
    OMIT_IDS = parse_ids(args.omit)
    EXTRA_FRAMES = parse_frames(args.add)
    run(args.channel, args.duration, args.trigger_delay, make_logger(args.output), parse_profile(args.current),
        args.cablecheck_hold, args.cablecheck_down,
        parse_profile(args.current_59c) if args.current_59c else None, taps=parse_taps(args.taps))


if __name__ == "__main__":
    main()
