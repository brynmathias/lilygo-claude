"""Print a keypads device's serial output for a few seconds.

Usage: uv run services/keypads/tools/serial_log.py [--port P] [--seconds N] [--send TEXT]

With no --port, uses the first /dev/cu.usbmodem* port.
"""

import argparse
import glob
import sys
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--seconds", type=float, default=8)
    ap.add_argument("--send", default="", help="text to write once the port is open")
    args = ap.parse_args()

    port = args.port or next(iter(sorted(glob.glob("/dev/cu.usbmodem*"))), None)
    if not port:
        print("no /dev/cu.usbmodem* port found", file=sys.stderr)
        return 1

    with serial.Serial(port, 115200, timeout=0.2) as s:
        print(f"# {port}", flush=True)
        if args.send:
            s.write(args.send.encode())
        end = time.monotonic() + args.seconds
        while time.monotonic() < end:
            line = s.readline()
            if line:
                print(line.decode(errors="replace").rstrip(), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
