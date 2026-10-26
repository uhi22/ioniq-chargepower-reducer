"""Observe CAN traffic with a PEAK PCAN adapter.

Prints a per-ID summary (count, rate, last data), similar to the serial output
of the board. With --raw, every frame is printed as it arrives.

Requires: pip install python-can, and the PEAK driver (PCANBasic.dll).

Usage:
  python tools/canmonitor.py                      # 5 s summary on PCAN_USBBUS1, 500 kBaud
  python tools/canmonitor.py -d 10 --raw
  python tools/canmonitor.py -c PCAN_USBBUS2
"""
import argparse
import time

import can


def main():
    parser = argparse.ArgumentParser(description="Observe CAN traffic with a PEAK PCAN adapter")
    parser.add_argument("-c", "--channel", default="PCAN_USBBUS1", help="PCAN channel (default PCAN_USBBUS1)")
    parser.add_argument("-b", "--bitrate", type=int, default=500000, help="bitrate (default 500000)")
    parser.add_argument("-d", "--duration", type=float, default=5.0, help="observation time in s (default 5)")
    parser.add_argument("--raw", action="store_true", help="print every received frame")
    args = parser.parse_args()

    stats = {}  # id -> [count, last frame]
    with can.Bus(interface="pcan", channel=args.channel, bitrate=args.bitrate) as bus:
        print(f"Listening on {args.channel} at {args.bitrate} bit/s for {args.duration} s ...")
        t_start = time.monotonic()
        t_end = t_start + args.duration
        while True:
            remaining = t_end - time.monotonic()
            if remaining <= 0:
                break
            msg = bus.recv(timeout=remaining)
            if msg is None or msg.is_error_frame:
                continue
            if args.raw:
                print(f"{time.monotonic() - t_start:9.3f}  0x{msg.arbitration_id:03X}  "
                      f"[{msg.dlc}]  {msg.data.hex(' ').upper()}")
            entry = stats.setdefault(msg.arbitration_id, [0, None])
            entry[0] += 1
            entry[1] = msg

    total = sum(e[0] for e in stats.values())
    print(f"\n{len(stats)} IDs, {total} frames, {total / args.duration:.1f} frames/s")
    print("ID      count   rate/s  dlc  last data")
    for can_id in sorted(stats):
        count, msg = stats[can_id]
        print(f"0x{can_id:03X}  {count:7d}  {count / args.duration:7.1f}  {msg.dlc:3d}  "
              f"{msg.data.hex(' ').upper()}")


if __name__ == "__main__":
    main()
