"""Decode targeted-mode ('t') sniffer logs.

  python tools/tframes.py LOG [LOG ...] [--raw]

In targeted mode the nRF24 matches the remote's real on-air address, so each
line holds the bytes that follow it. They are XN297-descrambled (scramble
table offset 5 = after a 5-byte address) and bit-reversed, then tallied per
'# label'. --raw also lists every frame with its timestamp.
"""
import collections
import re
import sys

from xn297_decode import BR, SCRAMBLE

TLINE = re.compile(r"^\[\s*(\d+)\]\s+ch(\d+) r(\d+) T:\s+((?:[0-9A-Fa-f]{2} ?)+)")
ALEN = 5
SHOW = 8  # bytes decoded per frame; the frame itself is 6, the rest is noise


def parse(path):
    label = ""
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if line.startswith("#"):
            label = line
            continue
        m = TLINE.match(line)
        if m:
            air = bytes.fromhex(m.group(4).replace(" ", ""))[:SHOW]
            yield label, int(m.group(1)), int(m.group(2)), bytes(BR[b ^ SCRAMBLE[ALEN + k]] for k, b in enumerate(air))


def main():
    raw = "--raw" in sys.argv
    for path in [a for a in sys.argv[1:] if not a.startswith("--")]:
        print(f"== {path}")
        by = collections.OrderedDict()
        for label, t, ch, pay in parse(path):
            by.setdefault(label, []).append((t, ch, pay))
        for label, rows in by.items():
            c = collections.Counter(p[:6] for _, _, p in rows)
            print(f"{label or '-'}: {len(rows)} frames")
            for k, n in c.most_common(12):
                print(f"    {k.hex(' ').upper()}  x{n}")
            if raw:
                for t, ch, p in rows:
                    print(f"    {t:>9} ch{ch:<3} {p[:6].hex(' ').upper()} | {p[6:].hex(' ').upper()}")


if __name__ == "__main__":
    main()
