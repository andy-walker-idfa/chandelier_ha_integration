"""Offline XN297 decoder for raw sniffer logs.

  python tools/xn297_decode.py captures/on_raw.log [more.log ...]

Each raw line is 32 bytes the nRF24 clocked in after a bait address match, at an
unknown bit offset. For every bit offset / address length / payload length we
test the CRC16 of the XN297 packet format (tables and layout taken from
DIY-Multiprotocol-TX-Module, XN297_EMU.ino), scrambled and unscrambled,
normal and enhanced (PCF) framing. Only CRC-valid frames are reported.
"""
import collections
import re
import sys

SCRAMBLE = [
    0xE3, 0xB1, 0x4B, 0xEA, 0x85, 0xBC, 0xE5, 0x66, 0x0D, 0xAE, 0x8C, 0x88, 0x12,
    0x69, 0xEE, 0x1F, 0xC7, 0x62, 0x97, 0xD5, 0x0B, 0x79, 0xCA, 0xCC, 0x1B, 0x5D,
    0x19, 0x10, 0x24, 0xD3, 0xDC, 0x3F, 0x8E, 0xC5, 0x2F, 0xAA, 0x16, 0xF3, 0x95]
XOROUT_SCR = [
    0x0000, 0x3448, 0x9BA7, 0x8BBB, 0x85E1, 0x3E8C, 0x451E, 0x18E6, 0x6B24,
    0xE7AB, 0x3828, 0x814B, 0xD461, 0xF494, 0x2503, 0x691D, 0xFE8B, 0x9BA7,
    0x8B17, 0x2920, 0x8B5F, 0x61B1, 0xD391, 0x7401, 0x2138, 0x129F, 0xB3A0,
    0x2988, 0x23CA, 0xC0CB, 0x0C6C, 0xB329, 0xA0A1, 0x0A16, 0xA9D0]
XOROUT_PLAIN = [
    0x0000, 0x3D5F, 0xA6F1, 0x3A23, 0xAA16, 0x1CAF, 0x62B2, 0xE0EB, 0x0821,
    0xBE07, 0x5F1A, 0xAF15, 0x4F0A, 0xAD24, 0x5E48, 0xED34, 0x068C, 0xF2C9,
    0x1852, 0xDF36, 0x129D, 0xB17C, 0xD5F5, 0x70D7, 0xB798, 0x5133, 0x67DB,
    0xD94E, 0x0A5B, 0xE445, 0xE6A5, 0x26E7, 0xBDAB, 0xC379, 0x8E20]
XOROUT_SCR_ENH = [
    0x0000, 0x7EBF, 0x3ECE, 0x07A4, 0xCA52, 0x343B, 0x53F8, 0x8CD0, 0x9EAC,
    0xD0C0, 0x150D, 0x5186, 0xD251, 0xA46F, 0x8435, 0xFA2E, 0x7EBD, 0x3C7D,
    0x94E0, 0x3D5F, 0xA685, 0x4E47, 0xF045, 0xB483, 0x7A1F, 0xDEA2, 0x9642,
    0xBF4B, 0x032F, 0x01D2, 0xDC86, 0x92A5, 0x183A, 0xB760, 0xA953]
XOROUT_PLAIN_ENH = [
    0x0000, 0x8BE6, 0xD8EC, 0xB87A, 0x42DC, 0xAA89, 0x83AF, 0x10E4, 0xE83E,
    0x5C29, 0xAC76, 0x1C69, 0xA4B2, 0x5961, 0xB4D3, 0x2A50, 0xCB27, 0x5128,
    0x7CDB, 0x7A14, 0xD5D2, 0x57D7, 0xE31D, 0xCE42, 0x648D, 0xBF2D, 0x653B,
    0x190C, 0x9117, 0x9A97, 0xABFC, 0xE68E, 0x0DE7, 0x28A2, 0x1965]

BR = [int(f"{i:08b}"[::-1], 2) for i in range(256)]


def crc_bits(crc, bits):
    for bit in bits:
        if ((crc >> 15) & 1) ^ bit:
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF
        else:
            crc = (crc << 1) & 0xFFFF
    return crc


def byte_at(bits, pos):
    v = 0
    for b in bits[pos:pos + 8]:
        v = (v << 1) | b
    return v


def decode(raw):
    """Yield (variant, bit_offset, addr, payload, extra) for every CRC-valid frame."""
    bits = [(raw[i >> 3] >> (7 - (i & 7))) & 1 for i in range(len(raw) * 8)]
    n = len(bits)
    for off in range(n - 40):
        # running CRC over on-air bits from this offset
        maxb = (n - off) // 8
        for scr in (True, False):
            for alen in (3, 4, 5):
                # --- normal framing: addr | payload | crc16
                crc = crc_bits(0xB5D2, bits[off:off + alen * 8])
                for plen in range(1, 33):
                    total = alen + plen
                    if total + 2 > maxb:
                        break
                    p = off + (total - 1) * 8
                    crc = crc_bits(crc, bits[p:p + 8])
                    idx = alen - 3 + plen
                    if idx >= len(XOROUT_SCR):
                        break
                    want = crc ^ (XOROUT_SCR if scr else XOROUT_PLAIN)[idx]
                    got = (byte_at(bits, off + total * 8) << 8) | byte_at(bits, off + total * 8 + 8)
                    if want == got:
                        air = [byte_at(bits, off + 8 * i) for i in range(total)]
                        if scr:
                            air = [a ^ SCRAMBLE[i] for i, a in enumerate(air)]
                        addr = bytes(reversed(air[:alen]))
                        pay = bytes(BR[a] for a in air[alen:])
                        yield ("scr" if scr else "plain", off, addr, pay, "")
                # --- enhanced framing: addr | pcf(10 bit: len7,pid2,noack1) | payload | crc16, bit packed
                pcf_pos = off + alen * 8
                if pcf_pos + 10 > n:
                    continue
                sbits = []
                for i in range(alen, len(SCRAMBLE)):
                    sbits += [(SCRAMBLE[i] >> (7 - k)) & 1 for k in range(8)]
                pcf = [bits[pcf_pos + i] ^ (sbits[i] if scr else 0) for i in range(10)]
                plen = int("".join(map(str, pcf[:7])), 2)
                if not 0 < plen <= 32:
                    continue
                idx = alen - 3 + plen
                end = pcf_pos + 10 + plen * 8
                if idx >= len(XOROUT_SCR_ENH) or end + 16 > n:
                    continue
                crc = crc_bits(0xB5D2, bits[off:end])
                want = crc ^ (XOROUT_SCR_ENH if scr else XOROUT_PLAIN_ENH)[idx]
                got = int("".join(map(str, bits[end:end + 16])), 2)
                if want == got:
                    air = [byte_at(bits, off + 8 * i) for i in range(alen)]
                    if scr:
                        air = [a ^ SCRAMBLE[i] for i, a in enumerate(air)]
                    addr = bytes(reversed(air))
                    pay = []
                    for i in range(plen):
                        seg = bits[pcf_pos + 10 + 8 * i: pcf_pos + 18 + 8 * i]
                        if scr:
                            seg = [b ^ sbits[10 + 8 * i + k] for k, b in enumerate(seg)]
                        pay.append(BR[int("".join(map(str, seg)), 2)])
                    extra = f"pid={pcf[7] * 2 + pcf[8]} noack={pcf[9]}"
                    yield ("scr-enh" if scr else "plain-enh", off, addr, bytes(pay), extra)


MIN_REPEAT = 3

LINE = re.compile(r"^\[\s*(\d+)\]\s+ch(\d+) r(\d+) a(\d+):\s+((?:[0-9A-Fa-f]{2} ?){32})")


def main():
    for path in sys.argv[1:]:
        label = ""
        raw_n = 0
        seen = collections.OrderedDict()
        for line in open(path, encoding="utf-8", errors="replace"):
            line = line.strip()
            if line.startswith("#"):
                label = line
                continue
            m = LINE.match(line)
            if not m:
                continue
            raw_n += 1
            raw = bytes.fromhex(m.group(5).replace(" ", ""))
            for var, off, addr, pay, extra in decode(raw):
                key = (label, var, addr.hex().upper(), pay.hex(" ").upper(), extra)
                e = seen.setdefault(key, {"n": 0, "offs": collections.Counter(), "t0": int(m.group(1))})
                e["n"] += 1
                e["offs"][off] += 1
        print(f"== {path}: {raw_n} raw packets, {len(seen)} distinct CRC-valid frames")
        for (label, var, addr, pay, extra), e in seen.items():
            if e["n"] < MIN_REPEAT:  # a lone CRC match is a chance hit on noise
                continue
            offs = ",".join(str(o) for o, _ in e["offs"].most_common(3))
            print(f"{label or '-':<14} {var:<9} x{e['n']:<4} t={e['t0']:<8} addr {addr} "
                  f"| payload {pay} {extra} (bit off {offs})")


if __name__ == "__main__":
    main()
