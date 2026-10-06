"""Scripted sniffer sessions.

  python tools/sweep.py prep                 reset, raw mode, background scan (do NOT press)
  python tools/sweep.py sweep LOG [LABEL]    press-scan, then ch 48..52 x rates 0..2 (press ~1/s)

Assumes 'prep' ran first (device left in: XN297 decode off, filter off, sniffing off).
"""
import sys
import time

import serial

from cap import open_port

PORT = "COM6"


class Dev:
    def __init__(self, log=None):
        self.s = open_port(PORT)
        self.log = open(log, "a", encoding="utf-8") if log else None

    def pump(self, secs):
        end = time.time() + secs
        while time.time() < end:
            try:
                data = self.s.read(4096)
            except serial.SerialException:
                self.s = open_port(PORT)
                continue
            if data:
                text = data.decode("utf-8", "replace").replace("\r\n", "\n")
                if self.log:
                    self.log.write(text)
                    self.log.flush()
                else:
                    sys.stdout.write(text)

    def cmd(self, c, wait=0.25):
        self.s.write((c + "\n").encode("utf-8"))
        self.pump(wait)

    def reset(self):
        self.s.rts = True
        self.s.dtr = self.s.dtr
        time.sleep(0.1)
        self.s.rts = False
        self.s.dtr = self.s.dtr
        self.pump(4)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    mode = sys.argv[1]
    if mode == "prep":
        d = Dev()
        d.reset()
        d.cmd("x")
        d.cmd("f")
        d.cmd("b", 16)
    elif mode == "sweep":
        d = Dev(sys.argv[2])
        label = sys.argv[3] if len(sys.argv) > 3 else "ON"
        d.cmd("# SCAN " + label)
        d.cmd("s", 16)
        d.cmd("a 3")
        d.cmd("p")  # sniffing on
        for rate in (0, 1, 2):
            for ch in (48, 49, 50, 51, 52):
                d.cmd("p")  # off while retuning, so the label lands between blocks
                d.cmd(f"r {rate}")
                d.cmd(f"c {ch}")
                d.cmd(f"# {label} r{rate} c{ch}")
                d.cmd("p", 3.0)
        d.cmd("p")  # sniffing off
    elif mode == "addr":
        # sweep.py addr LOG LABEL RATE CH [SECS]: try all 4 bait addresses
        d = Dev(sys.argv[2])
        label, rate, ch = sys.argv[3], sys.argv[4], sys.argv[5]
        secs = float(sys.argv[6]) if len(sys.argv) > 6 else 6
        d.cmd(f"r {rate}")
        d.cmd(f"c {ch}")
        for a in (0, 1, 2, 3):
            d.cmd(f"a {a}")
            d.cmd(f"# {label} r{rate} c{ch} a{a}")
            d.cmd("p", secs)
            d.cmd("p")
    elif mode == "tchans":
        # sweep.py tchans LOG LABEL SECS FIRST LAST: targeted mode channel sweep.
        # Resets the board first, so the starting state is known.
        d = Dev(sys.argv[2])
        label, secs = sys.argv[3], float(sys.argv[4])
        d.reset()
        d.cmd("r 2")
        d.cmd("t")
        for ch in range(int(sys.argv[5]), int(sys.argv[6]) + 1):
            d.cmd(f"c {ch}", 0.06)
            d.cmd(f"# {label} c{ch}", 0.06)
            d.cmd("p", secs)
            d.cmd("p", 0.06)
    elif mode == "chans":
        # sweep.py chans LOG LABEL SECS CH [CH ...]: 250 kbps, bait 0, dwell on each channel
        d = Dev(sys.argv[2])
        label, secs = sys.argv[3], float(sys.argv[4])
        d.cmd("r 2")
        d.cmd("a 0")
        for ch in sys.argv[5:]:
            d.cmd(f"c {ch}", 0.1)
            d.cmd(f"# {label} c{ch}", 0.1)
            d.cmd("p", secs)
            d.cmd("p", 0.1)


if __name__ == "__main__":
    main()
