#!/usr/bin/env python3
"""
Push the host's wall-clock time into the device's RX8130 RTC over serial.

    tools/set_time.py                    # autodetect the port
    tools/set_time.py -p /dev/ttyACM0

Why this exists: the device has no network by design, so it cannot use NTP.
The RTC keeps local wall-clock time (the firmware pins TZ=UTC0 so nothing is
converted), and this sends exactly what `date` says.

Run it after flashing. Until you do, the clock shows the firmware build stamp
and the status bar draws the time in red with a '?' so the guess is obvious.
"""

import argparse
import glob
import sys
import time
from datetime import datetime

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial missing:  pip install pyserial\n"
             "(ESP-IDF's own venv already has it: "
             "~/.espressif/python_env/idf5.5_py3.13_env/bin/python)")


def autodetect():
    # The ESP32-S3's USB-Serial-JTAG shows up as an ACM device.
    for pattern in ("/dev/serial/by-id/*Espressif*", "/dev/ttyACM*"):
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[0]
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", default=None)
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--no-repaint", action="store_true",
                    help="skip the follow-up repaint (saves a ~10s refresh)")
    args = ap.parse_args()

    port = args.port or autodetect()
    if not port:
        sys.exit("no serial port found; pass -p /dev/ttyACMx")

    now = datetime.now()
    stamp = now.strftime("%Y-%m-%d %H:%M:%S")
    print(f"port : {port}")
    print(f"time : {stamp}  (local wall clock)")

    # Do NOT touch DTR/RTS: on USB-Serial-JTAG those lines are wired to
    # EN/GPIO0, and asserting them reboots the chip into ROM download mode --
    # which is exactly the trap that makes the device look bricked.
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = args.baud
    ser.timeout = 1.0
    ser.dsrdtr = False
    ser.rtscts = False
    ser.open()

    try:
        time.sleep(0.3)
        ser.reset_input_buffer()

        # A bare newline first so the REPL discards any partial line.
        ser.write(b"\r\n")
        time.sleep(0.2)
        ser.write(f"time {stamp}\r\n".encode())
        ser.flush()

        deadline = time.time() + 3.0
        out = b""
        while time.time() < deadline:
            chunk = ser.read(256)
            if chunk:
                out += chunk
                deadline = time.time() + 0.5
        text = out.decode("utf-8", "replace")
        print("--- device said ---")
        print(text.strip() or "(no response -- is the firmware running?)")
        print("-------------------")

        if "time set" in text:
            print("RTC set.")
        else:
            print("WARNING: no confirmation seen. Check `time` manually over "
                  "`idf.py monitor`.")

        if not args.no_repaint:
            print("requesting a repaint (~10s panel refresh)...")
            ser.write(b"repaint\r\n")
            ser.flush()
            time.sleep(1.0)
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
