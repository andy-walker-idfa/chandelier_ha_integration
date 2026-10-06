"""Extract the chandelier remote's frames from raw 250 kbps sniffer logs.

  python tools/extract.py LOG [LOG ...]

Finds the scrambled on-air address (2F 7D 87 BF 2F = CC CC CC 55 AA after
XN297 descrambling) at any bit offset, descrambles and bit-reverses the bytes
that follow, and prints the frames grouped into bursts (gap > 1.5 s = new
button press group) with a majority vote per burst.
"""
import collections
import sys

from xn297_decode import BR, LINE, SCRAMBLE

AIR_ADDR = "".join(f"{b:08b}" for b in (0x2F, 0x7D, 0x87, 0xBF, 0x2F))
ALEN = 5
PLEN = 10  # bytes shown after the address; the real frame is shorter, the rest is noise


def frames(path):
    label = ""
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if line.startswith("#"):
            label = line
            continue
        m = LINE.match(line)
        if not m:
            continue
        bits = "".join(f"{b:08b}" for b in bytes.fromhex(m.group(5).replace(" ", "")))
        i = bits.find(AIR_ADDR)
        if i < 0:
            continue
        body = bits[i + 40:]
        n = min(PLEN, len(body) // 8)
        pay = bytes(BR[int(body[8 * k:8 * k + 8], 2) ^ SCRAMBLE[ALEN + k]] for k in range(n))
        yield int(m.group(1)), label, pay


def main():
    for path in sys.argv[1:]:
        print(f"== {path}")
        groups = []
        last_t = None
        for t, label, pay in frames(path):
            if last_t is None or t - last_t > 1500 or groups[-1]["label"] != label:
                groups.append({"label": label, "t0": t, "rows": []})
            groups[-1]["rows"].append(pay)
            groups[-1]["t1"] = t
            last_t = t
        for g in groups:
            full = [r for r in g["rows"] if len(r) >= 6]
            c = collections.Counter(r[:6] for r in full)
            desc = "  ".join(f"{k.hex(' ').upper()} x{n}" for k, n in c.most_common(4))
            tails = collections.Counter(r[6:8].hex().upper() for r in full if len(r) >= 8)
            print(f"{g['label'] or '-':<22} t={g['t0'] / 1000:7.1f}-{g['t1'] / 1000:7.1f}s "
                  f"frames {len(g['rows']):<3} | {desc} | next2 {dict(tails.most_common(3))}")


if __name__ == "__main__":
    main()
