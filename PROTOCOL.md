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
| Address | **5 bytes: `AA 55 CC CC CC`** (logical, de-scrambled). Identical on both remotes captured, so it appears fixed for this product; the per-remote identity is in the payload ([Remotes](#remotes)). |
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
| Meaning | `phase` | ID | ID | ID | ID | `command` | `counter` | `checksum` |

- **`phase`** — `00` for the first burst of a press, then `01`, `02`, … for
  each further burst while the button is held (see
  [Two-burst behavior](#two-burst-behavior)).
- **Bytes 1–4: remote ID.** Constant across all buttons of one remote and
  different between remotes. Of the two remotes captured, only bytes 2–3 differ;
  bytes 1 (`55`) and 4 (`00`) match. Whether those two are part of the ID or a
  fixed header can't be told from two samples, so all four are treated as the ID
  field. See [Remotes](#remotes).
- **`command`** — the button, see [Commands](#commands).
- **`counter`** — a one-byte rolling counter, **+1 per burst** (so each press
  consumes two consecutive values). Wraps `FF → 00`.
- **`checksum`** — `sum(bytes 0..6) mod 256`.

### Remotes

Two remotes (one per chandelier) have been captured. Everything below was
verified frame by frame with the XN297 CRC and the payload checksum.

| | Remote 1 | Remote 2 |
|---|---|---|
| Radio address | `AA 55 CC CC CC` | `AA 55 CC CC CC` (**same**) |
| ID bytes (payload 1–4) | `55 2A 75 00` | `55 5D 73 00` (**different**) |
| Data rate / channel tested | 250 kbps, hops incl. ch 50 | 250 kbps, ch 50 |
| Command bytes | ON `05`, OFF `09`, NIGHT `10`, DAY `11`, TEMP `07` | identical |
| Phase ≥ 01 command | `command + 0x40` | identical |
| Checksum / CRC | sum of bytes 0–6; XN297 CRC-16 | identical |
| Frames verified | hundreds | 636 (635 CRC-valid, all checksum-valid) |

Remote 2's full channel-hopping list wasn't swept. It was received on channel 50
at the same rate and address, so it is assumed to use the same hop set.

**What this means for independent control.** Both remotes share the radio
address, so a receiver can't tell them apart by address. The lamps can tell them
apart only by the ID bytes in the payload. Since the IDs differ, **independent
radio control of the two chandeliers is possible in principle**: a transmitter
can send either remote's ID.

There's a caveat. The original problem was that **one remote currently controls
both chandeliers**, which means at least one lamp accepts more than one remote
ID. Which lamp responds to which ID is set by the lamps' pairing, and sniffing
the remotes can't reveal it. Whether the two lamps can be separated therefore
still has to be tested at the lamps; see [Open questions](#open-questions).

### Commands

| Button | `command` byte (phase 00) | phase ≥ 01 (`+0x40`) | Semantics |
|--------|---------------------------|----------------------|-----------|
| ON         | `05` | `45` | Absolute (sets a state) |
| OFF        | `09` | `49` | Absolute |
| NIGHT      | `10` | `50` | *Pending verification* |
| DAY        | `11` | `51` | *Pending verification* |
| TEMP cycle | `07` | `47` | **Relative** — advances to the next color-temperature preset |

**Absolute vs relative.** An *absolute* command puts the lamp into a known state
no matter what it was doing, so sending it twice is harmless. A *relative*
command changes the state based on the current one. The TEMP-cycle button is
relative: each press advances to the next preset in a loop, and the protocol
carries **no absolute color-temperature value**. A controller cannot read back or
directly select a preset; it can only count presses from a known starting point.
For the same reason a transmitter must not repeat a relative press with new
counter values, since each repeat may advance the preset again.

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

**The phase byte appears to count hold-repeats.** It is not always exactly two
bursts. Very quick taps were captured with only a phase-00 burst, and a TEMP
press held slightly longer produced a third burst, phase `02`:

```
00 55 2A 75 00 07 2A <cs>    # press
01 55 2A 75 00 47 2B <cs>    # +200 ms
02 55 2A 75 00 47 2C <cs>    # +200 ms
```

The working theory is that the remote sends one burst every ~200 ms while the
button is held, with `phase` incrementing each time and the command carrying
`+0x40` from phase 01 onward. A transmitter sending phase 00 + phase 01 matches a
normal short press. What a long hold (many phases) does on the lamp is untested.

### Worked example

A real ON press from remote 1, captured at 250 kbps on channel 50:

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
- **Which lamp accepts which remote ID?** *(Partly answered.)* The second remote
  has been captured: same radio address, **different ID** (`55 5D 73 00` vs
  `55 2A 75 00`). What's still unknown is the lamps' pairing. Since one remote
  currently drives both chandeliers, at least one lamp accepts both IDs. To
  find out, transmit with each ID in turn and note which lamp(s) react. If both
  lamps respond to one ID, the lamps will need re-pairing (or a new ID per lamp)
  before they can be controlled independently. Many lamps of this type have a
  re-pair procedure, but none has been tested on these.
- **Are ID bytes 1 and 4 part of the ID?** Both remotes have `55` and `00` there;
  more remotes would be needed to tell.

## How this was captured

See [`captures/README.md`](captures/README.md) for the raw and decoded logs and
the exact serial commands used to produce each one.
