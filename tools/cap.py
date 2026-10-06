"""Serial helper for the nRF24 sniffer.

  python tools/cap.py [--reset] [--secs N] [--log FILE] [--port COM6] CMD [CMD ...]

Sends each CMD as a line (0.4 s apart), then prints/logs everything the board
sends for N seconds. --reset pulses RTS first to reboot the board (to see the banner).
"""
import argparse
import os
import sys
import time

import serial


def open_port(port, tries=40):
    for _ in range(tries):
        try:
            s = serial.Serial()
            s.port, s.baudrate, s.timeout = port, 115200, 0.1
            s.dtr = False
            s.rts = False
            s.open()
            return s
        except serial.SerialException:
            time.sleep(0.05)
    sys.exit(f"cannot open {port}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM6")
    ap.add_argument("--secs", type=float, default=3)
    ap.add_argument("--log")
    ap.add_argument("--reset", action="store_true")
    ap.add_argument("--stop-file", help="stop early once this file exists")
    ap.add_argument("cmds", nargs="*")
    a = ap.parse_args()

    s = open_port(a.port)
    if a.reset:
        try:
            # Windows usbser only pushes RTS to the device when DTR is written too
            s.rts = True
            s.dtr = s.dtr
            time.sleep(0.1)
            s.rts = False
            s.dtr = s.dtr
        except serial.SerialException:
            pass

    log = open(a.log, "a", encoding="utf-8") if a.log else None
    out = sys.stdout.buffer

    def pump(until):
        nonlocal s
        while time.time() < until:
            if a.stop_file and os.path.exists(a.stop_file):
                return
            try:
                data = s.read(4096)
            except serial.SerialException:  # native USB re-enumerated after reset
                try:
                    s.close()
                except Exception:
                    pass
                s = open_port(a.port)
                continue
            if data:
                out.write(data)
                out.flush()
                if log:
                    log.write(data.decode("utf-8", "replace").replace("\r\n", "\n"))
                    log.flush()

    pump(time.time() + (4.0 if a.reset else 0.3))
    for c in a.cmds:
        s.write((c + "\n").encode("utf-8"))
        pump(time.time() + 0.4)
    pump(time.time() + a.secs)
    s.close()


if __name__ == "__main__":
    main()
