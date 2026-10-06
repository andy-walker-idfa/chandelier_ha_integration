"""Per-label statistics for a raw sniffer log: packet count, how many look
saturated (long constant runs = receiver off-frequency) and the most repeated
non-trivial 40-bit patterns (a real, correctly received packet repeats)."""
import collections
import re
import sys

from xn297_decode import LINE

TRIVIAL = re.compile(r"0{14}|1{14}|(01){9}")


def main():
    blocks = collections.OrderedDict()
    label = "-"
    for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
        line = line.strip()
        if line.startswith("#"):
            label = line
            continue
        m = LINE.match(line)
        if m:
            blocks.setdefault(label, []).append(bytes.fromhex(m.group(5).replace(" ", "")))
    for label, rows in blocks.items():
        sat = 0
        cnt = collections.Counter()
        for r in rows:
            b = "".join(f"{x:08b}" for x in r)
            if re.search(r"0{40}|1{40}", b):
                sat += 1
            cnt.update({b[i:i + 40] for i in range(256 - 40) if not TRIVIAL.search(b[i:i + 40])})
        top = [(f"{int(k, 2):010X}", n) for k, n in cnt.most_common(3)]
        print(f"{label:<16} pkts {len(rows):<5} saturated {sat:<5} top40 {top}")


if __name__ == "__main__":
    main()
