# XN297L LED chandelier remote — RF protocol

This document describes the 2.4 GHz radio protocol of a cheap LED chandelier's
handheld remote, decoded with an nRF24L01+ in promiscuous mode. All values here
were **verified against hundreds of captured frames by CRC**; anything not yet
confirmed is listed under [Open questions](#open-questions).

If you arrived here searching for *"XN297 LED lamp remote protocol"*: the encoding
below is the standard Nordic-clone XN297 (as used by many toys and the
[DIY-Multiprotocol-TX-Module](https://github.com/pascallanger/DIY-Multiprotocol-TX-Module)
project). Only the **address, command bytes and payload layout** are specific to
this chandelier.

## Radio parameters

| Parameter | Value |
|-----------|-------|
| Chip family | XN297 / XN297L (Nordic nRF24L01 clone with scrambling) |
| Modulation / data rate | GFSK, **250 kbps** |
| Scrambling | **On** (standard XN297 scramble table) |
| Preamble | `71 0F 55` |
| Address | **5 bytes: `AA 55 CC CC CC`** (logical, de-scrambled) |
| Payload length | 8 bytes |
| CRC | **XN297 CRC-16** (CCITT poly `0x1021`, init `0xB5D2`, XN297 xorout table) |
| Channel hopping | Yes — see [Channels](#channels) |

### On-air encoding

Each on-air byte after the address is **bit-reversed and XORed** with the XN297
scramble table; the CRC is computed over the *scrambled* address+payload bytes and
then XORed with the XN297 xorout value for that length. The receiver reverses this:

```
logical_payload[i] = bitreverse( onair_payload[i] XOR scramble[addr_len + i] )
crc = xn297_crc(onair_address .. onair_payload) XOR xorout[addr_len - 3 + payload_len]
```

Reference scramble table and xorout tables are embedded in
[`firmware/sniffer/src/main.cpp`](firmware/sniffer/src/main.cpp) and in
[`tools/xn297_decode.py`](tools/xn297_decode.py).

### Channels

The remote hops across this channel list (nRF24 channel number = 2400 + N MHz):

```
0  2  5  18  21  34  37  45  47  50  53  66  69  82
```

Channel **50** (2450 MHz) is confirmed active and convenient for bench testing.
During a button press the remote sends a burst of roughly **10 frames, ~13 ms
apart**, hopping as it goes; a receiver parked on any one of these channels will
catch a subset of the burst.

## Payload layout

The 8-byte logical payload:

| Byte | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|------|---|---|---|---|---|---|---|---|
| Meaning | `phase` | `55` | `2A` | `75` | `00` | `command` | `counter` | `checksum` |

- **`phase`** — `00` for the first burst of a press, `01` for the second burst
  (see [Two-burst behavior](#two-burst-behavior)).
- **Bytes 1–4 = `55 2A 75 00`** — constant across all buttons on this remote;
  treated as the **remote/device ID**.
- **`command`** — the button, see [Commands](#commands).
- **`counter`** — a one-byte rolling counter, **+1 per burst** (so each press
  consumes two consecutive values). Wraps `FF → 00`.
- **`checksum`** — `sum(bytes 0..6) mod 256`.

### Commands

| Button | `command` byte (phase 00) | phase 01 (`+0x40`) |
|--------|---------------------------|--------------------|
| ON     | `05` | `45` |
| OFF    | `09` | `49` |
| NIGHT  | `10` | `50` |
| DAY    | `11` | `51` |

### Two-burst behavior

A single button press transmits **two bursts**:

1. **Phase 00 burst** — ~10 frames, `phase = 00`, `command` as above, one counter
   value held constant across the burst.
2. **~200 ms gap.**
3. **Phase 01 burst** — ~10 frames, `phase = 01`, **`command | 0x40`**, counter
   incremented by one.

So pressing ON once sends, e.g.:

```
phase 00:  00 55 2A 75 00 05 <ctr>   <checksum>
phase 01:  01 55 2A 75 00 45 <ctr+1> <checksum>
```

### Worked example

A real ON press captured at 250 kbps on channel 50:

```
00 55 2A 75 00 05 F5 EE      # phase 00, cmd 05 (ON),  ctr F5, checksum EE
01 55 2A 75 00 45 F6 30      # phase 01, cmd 45,       ctr F6, checksum 30
```

Checksum check for the first frame:
`00+55+2A+75+00+05+F5 = 0x1EE`, and `0x1EE mod 256 = 0xEE`. ✓

## Open questions

These are **not yet verified** and are the current focus of work:

- **Does the lamp enforce the rolling counter?** It is unknown whether the
  chandelier rejects a replayed/old counter value. If it does, a transmitter must
  track and advance the counter; if not, any valid frame works. (Note: during
  testing the counter was observed to **wrap past `0xFF` and still be accepted by
  the lamp via the real remote**, which hints at loose or no enforcement — but
  this has not been confirmed by a controlled replay yet.)
- **Is the channel list complete?** The 14 channels above were found by a sweep
  with short dwell times; there may be more.
- **Do the second remote / second chandelier share the same ID?** Bytes `55 2A
  75 00` may be per-remote. A second remote might use a different ID (and that is
  likely the root of the "one remote controls both" pairing problem). Needs a
  capture from the second remote.

## How this was captured

See [`captures/README.md`](captures/README.md) for the raw and decoded logs and
the exact serial commands used to produce each one.
