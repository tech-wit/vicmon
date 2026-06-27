#!/usr/bin/env python3
"""Read an ESP32 serial port for a fixed duration and print what arrives.

A non-interactive alternative to `pio device monitor` (which needs a TTY).
Useful for capturing diagnostic output from the vicmon firmware.

Usage:
    python tools/monitor.py [--port /dev/ttyACM0] [--baud 115200] [--seconds 20]

Requires pyserial (installed in the project's .piovenv):
    .piovenv/bin/python tools/monitor.py
"""
import argparse
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial not found — run with .piovenv/bin/python")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default="/dev/ttyACM0", help="serial device")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=20.0,
                    help="how long to capture (0 = until interrupted)")
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as exc:
        sys.exit(f"could not open {args.port}: {exc}")

    time.sleep(0.2)
    ser.reset_input_buffer()

    deadline = None if args.seconds == 0 else time.time() + args.seconds
    try:
        while deadline is None or time.time() < deadline:
            data = ser.read(256)
            if data:
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
