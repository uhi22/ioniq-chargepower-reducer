"""Run one clean bench session: CCM power cycle, fresh pyPLC key, vehicle simulation.

Sequence:
  1. CCM (and MITM board) power off, via the iobox DOUT0 on RPi2023
  2. pyPLC restart with a new random key: stop (mode off), wait 1 s, start
     (infiniteSessionRandomKey), wait 5 s until the modem is re-keyed. So pyPLC waits
     for a fresh SLAC.
  3. CCM power on
  4. vehicle simulation (tools/vehiclesim.py)
  5. print the session progress from the new pyPLC run log

Requires: pip install python-can pyserial, the PEAK driver, and SSH key access to
pi@192.168.2.118 (RPi2023 with the pyPLC supervisor and the iobox via ser2net).

Usage:
  python tools/benchsession.py                     # 60 s simulation
  python tools/benchsession.py -d 90 -o session.txt
"""
import argparse
import re
import subprocess
import time

import serial

import vehiclesim

RPI = "pi@192.168.2.118"
IOBOX_URL = "socket://192.168.2.118:2000"
CCM_POWER_DOUT = 0
CCM_ON = 0   # inverted: relay normally-closed contact
CCM_OFF = 1
OFF_HOLD_S = 4.0
PYPLC_MODE = "infiniteSessionRandomKey"
PYPLC_STOP_WAIT_S = 1.0   # pause between pyPLC stop and start
PYPLC_REKEY_WAIT_S = 5.0  # from pyPLC start until the modem is re-keyed


def ssh(command, timeout=30):
    """Run a command on RPi2023, -> stdout."""
    result = subprocess.run(["ssh", "-o", "BatchMode=yes", RPI, command],
                            capture_output=True, text=True, timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(f"ssh '{command}' failed: {result.stderr.strip()}")
    return result.stdout


def ccm_power(on, log):
    level = CCM_ON if on else CCM_OFF
    with serial.serial_for_url(IOBOX_URL, baudrate=115200, timeout=1.0) as box:
        box.write(f"DOUT{CCM_POWER_DOUT}={level}\n".encode())
        reply = box.readline().decode(errors="replace").strip()
    if f'"DOUT{CCM_POWER_DOUT}"' not in reply:
        raise RuntimeError(f"iobox did not confirm DOUT{CCM_POWER_DOUT}={level}: {reply!r}")
    log(f"CCM power {'ON' if on else 'OFF'} (DOUT{CCM_POWER_DOUT}={level})")


def last_run():
    """-> number of the newest pyPLC run from ctl.log, 0 if none."""
    runs = re.findall(r"run=(\d+) start", ssh("cat /tmp/pyplc/ctl.log"))
    return int(runs[-1]) if runs else 0


def wait_ctl(pattern, timeout):
    """Wait until ctl.log contains pattern, -> the matching line."""
    t_end = time.monotonic() + timeout
    while time.monotonic() < t_end:
        for line in ssh("cat /tmp/pyplc/ctl.log").splitlines():
            if re.search(pattern, line):
                return line
        time.sleep(0.5)
    raise RuntimeError(f"timeout waiting for '{pattern}' in ctl.log")


def restart_pyplc(log, params=()):
    """Restart pyPLC with a new random key, -> path of the new run log.
    params: optional supervisor parameters, e.g. ['fastCableCheck=0']."""
    run = last_run()
    ssh("echo off > ~/bench/pyplc.mode")
    if run:
        wait_ctl(rf"run={run} end", 15)
    time.sleep(PYPLC_STOP_WAIT_S)
    ssh("printf '%s\n' " + " ".join([PYPLC_MODE, *params]) + " > ~/bench/pyplc.mode")
    line = wait_ctl(rf"run={run + 1} start", 15)
    t_start = time.monotonic()
    run_log = re.search(r"log=(\S+)", line).group(1)
    nmk = re.search(r"nmk=(\S+)", line).group(1)
    t_end = time.monotonic() + 15
    while "received SET_KEY.CNF" not in ssh(f"cat {run_log}"):
        if time.monotonic() > t_end:
            raise RuntimeError(f"pyPLC run {run + 1} did not configure its modem key")
        time.sleep(0.5)
    # the modem needs some seconds after SET_KEY until it is ready for SLAC
    time.sleep(max(0.0, PYPLC_REKEY_WAIT_S - (time.monotonic() - t_start)))
    log(f"pyPLC run {run + 1} ready, NMK {nmk}, log {run_log}")
    return run_log


def session_progress(run_log):
    """-> the SLAC and V2G milestones from the pyPLC run log."""
    lines = []
    for line in ssh(f"cat {run_log}").splitlines():
        if "received SLAC_PARAM.REQ" in line:
            lines.append("SLAC_PARAM")
        if "received SLAC_MATCH.REQ" in line:
            lines.append("SLAC_MATCH")
        m = re.search(r'"msgName": "(\w+)"', line)
        if m:
            lines.append(m.group(1))
    # collapse repetitions, e.g. 13 x SLAC_PARAM
    collapsed = []
    for name in lines:
        if collapsed and collapsed[-1][0] == name:
            collapsed[-1][1] += 1
        else:
            collapsed.append([name, 1])
    return [name if n == 1 else f"{n}x {name}" for name, n in collapsed]


def remaining_times(run_log):
    """-> the distinct (RemainingTimeToFullSoC, RemainingTimeToBulkSoC) values in the CurrentDemandReqs."""
    pairs = []
    full = None
    for line in ssh(f"cat {run_log}").splitlines():
        m = re.search(r'"RemainingTimeTo(Full|Bulk)SoC.Value": "(-?\d+)"', line)
        if not m:
            continue
        if m.group(1) == "Full":
            full = int(m.group(2))
        elif (full, int(m.group(2))) not in pairs:
            pairs.append((full, int(m.group(2))))
    return pairs


def target_current_changes(run_log):
    """-> one line per change of the EVTargetCurrent in the CurrentDemandReqs, with pyPLC time."""
    lines = []
    last = None
    t_first = None
    for line in ssh(f"cat {run_log}").splitlines():
        m = re.search(r"\[(\d+)ms\] \[EVSE\] EV wants EVTargetVoltage (\S+) and EVTargetCurrent (\S+)", line)
        if not m:
            continue
        t_ms, voltage, current = int(m.group(1)), m.group(2), m.group(3)
        if t_first is None:
            t_first = t_ms
        if current != last:
            last = current
            lines.append(f"  pyPLC t={(t_ms - t_first) / 1000:7.2f} s  EVTargetCurrent {current} A  (EVTargetVoltage {voltage} V)")
    return lines


def main():
    parser = argparse.ArgumentParser(description="Run one clean bench session with the CCM")
    parser.add_argument("-c", "--channel", default="PCAN_USBBUS1", help="PCAN channel (default PCAN_USBBUS1)")
    parser.add_argument("-d", "--duration", type=float, default=60.0, help="simulation time in s (default 60)")
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
    parser.add_argument("--pyplc", nargs="*", default=["fastCableCheck=0"],
                        help="pyPLC supervisor parameters (default fastCableCheck=0)")
    parser.add_argument("--fast", action="store_true",
                        help="fast test: end 2 s after the CCM reached CurrentDemand, or at once on an abort "
                             "(max 25 s), judged by the CCM progress on CAN")
    parser.add_argument("--omit", default="", help="IDs (hex, comma separated) which are not sent at all, e.g. 542,5D7")
    parser.add_argument("--soc", type=float, default=34.0, help="SOC in %% in 0x599 B2 (0.5 %% steps, default 34)")
    parser.add_argument("--taps", help="pedal taps t:kind,... (t = s after start of current demand; "
                                       "kind brake, accel (0.3 s), brakelong, accellong (2 s))")
    parser.add_argument("--add", action="append", metavar="ID:DATA",
                        help="additional static message every 100 ms, e.g. 596:0082000017001A1C (repeatable)")
    parser.add_argument("-o", "--output", help="also write the log to this file")
    args = parser.parse_args()
    log = vehiclesim.make_logger(args.output)
    vehiclesim.RAMP_A_PER_CYCLE = args.ramp
    vehiclesim.UCCS_MODE = args.uccs
    vehiclesim.SOC_PERCENT = args.soc
    vehiclesim.OMIT_IDS = vehiclesim.parse_ids(args.omit)
    vehiclesim.EXTRA_FRAMES = vehiclesim.parse_frames(args.add)

    ccm_power(False, log)
    t_off = time.monotonic()
    run_log = restart_pyplc(log, args.pyplc)
    time.sleep(max(0.0, OFF_HOLD_S - (time.monotonic() - t_off)))
    ccm_power(True, log)

    vehiclesim.run(args.channel, min(args.duration, 25.0) if args.fast else args.duration, args.trigger_delay,
                   log, vehiclesim.parse_profile(args.current),
                   args.cablecheck_hold, args.cablecheck_down,
                   vehiclesim.parse_profile(args.current_59c) if args.current_59c else None,
                   2.0 if args.fast else None, taps=vehiclesim.parse_taps(args.taps))

    # the pyPLC log lags behind real time, so give it some seconds to catch up
    time.sleep(1 if args.fast else 5)
    progress = session_progress(run_log)
    log("pyPLC session progress: " + (" -> ".join(progress) if progress else "(nothing)"))
    times = remaining_times(run_log)
    if times:
        log("RemainingTime (ToFullSoC, ToBulkSoC) in s: " + ", ".join(f"({f}, {u})" for f, u in times))
    for line in target_current_changes(run_log):
        log(line)


if __name__ == "__main__":
    main()
