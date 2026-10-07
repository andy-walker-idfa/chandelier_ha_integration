# XN297 remote analyzer (receive-only sniffer)

A standalone, **receive-only** firmware for reverse-engineering cheap 2.4 GHz
remotes built on Nordic **nRF24L01 / XN297 / XN297L** radios — the kind found in
budget LED lamps, ceiling fans and toys. It listens, de-scrambles the XN297
encoding, checks the CRC, and prints the decoded address, payload and command
bytes of your own remote so you can build your own controller.

This is the tool we used to decode the chandelier remotes in this project. It's
split out from the combined firmware (which also transmits and talks to Home
Assistant) so you can **reuse just the analysis part**: flash it, press your
remote, read off the bytes, and adapt it to your device. It never transmits, so
it can't command anything — it only observes.

> New to the project? Start here, then see the top-level
> [README](../../README.md), [PROTOCOL.md](../../PROTOCOL.md) (a fully worked
> example) and [HARDWARE.md](../../HARDWARE.md).

## What it does and why it works

XN297 is an nRF24L01 clone that adds two things on top of a plain nRF24 frame:
each byte after the preamble is **bit-reversed and XOR-ed with a fixed scramble
table**, and the packet ends with an **XN297 CRC-16**. A normal nRF24 can't
decode that directly, so this firmware:

1. Puts the nRF24 in a promiscuous mode (a 2-byte "bait" address that triggers on
   almost any traffic), capturing 32 raw bytes per hit.
2. Slides a decoder across **every bit offset**, trying each address length
   (3–5) and payload length (1–16), de-scrambling and checking the CRC.
3. Prints a frame **only when the CRC is valid** — which is near-proof that the
   decode is correct and the alignment is right.

Because the scramble table and CRC are standard across XN297 devices, this works
for other XN297 remotes too — only the address, payload layout and command bytes
are specific to each product, and those are exactly what it extracts.

## Hardware

- **ESP32-S3-DevKitC-1** (native USB). A classic ESP32 DevKit also works — the
  firmware auto-selects pins.
- **nRF24L01+** module (the plain PCB-antenna version is fine on a bench).
- **A 10–100 µF capacitor across the module's VCC/GND pins** — not optional with
  cheap clones; without it you get flaky SPI and "nRF24 not responding".
- A few jumper wires.

Full bill of materials and the schematic are in
[HARDWARE.md](../../HARDWARE.md). Wiring summary:

| nRF24L01+ | ESP32-S3 | classic ESP32 |
|-----------|----------|---------------|
| CE   | GPIO9  | GPIO4  |
| CSN  | GPIO10 | GPIO5  |
| SCK  | GPIO12 | GPIO18 |
| MOSI | GPIO11 | GPIO23 |
| MISO | GPIO13 | GPIO19 |
| VCC  | 3V3 (+ cap) | 3V3 (+ cap) |
| GND  | GND | GND |
| IRQ  | — | — |

To use different pins, edit the `#define PIN_*` block near the top of
[`src/main.cpp`](src/main.cpp).

## Flashing

[PlatformIO](https://platformio.org/) (CLI or the VS Code extension):

```bash
cd firmware/rx-sniffer
pio run -t upload                 # build + flash over USB (auto-detects the port)
pio run -t upload --upload-port COM6   # or name the port explicitly
pio device monitor                # open the serial console (115200 baud)
```

Notes:
- The ESP32-S3's **native USB** needs `-DARDUINO_USB_CDC_ON_BOOT=1` or the serial
  console stays empty; it's already set in [`platformio.ini`](platformio.ini).
- If upload fails with *port busy / access denied*, close any open serial monitor
  first — it holds the port.
- In the monitor, set the line ending to **Newline** so your commands are seen.

No Arduino IDE required, but if you prefer it: open `src/main.cpp`, select the
ESP32-S3 board, and add `ARDUINO_USB_CDC_ON_BOOT=1` to the build defines.

## Serial commands

| Cmd | Action |
|-----|--------|
| `b` | Background channel scan — **don't** touch the remote (~10 s) |
| `s` | Activity scan **while** you hold a button (~10 s) |
| `c N` | Set channel N (0–125); frequency = 2400 + N MHz |
| `r N` | Data rate: `0`=1 Mbps, `1`=2 Mbps, `2`=250 kbps |
| `a N` | Bait-address variant 0–3 (promiscuous capture) |
| `f` | Toggle "show repeats only" for raw dumps |
| `x` | Toggle the XN297 decoder (off = raw 32-byte hex) |
| `t` | Toggle targeted mode (match one known address — see below) |
| `p` | Start / stop sniffing |
| `# text` | Print a label line (handy when logging button presses) |
| `?` | Help + current settings |

## Step-by-step: analyze your own remote

1. **Find the channel(s).** Flash, open the monitor, type `b` with the remote
   idle (records background noise). Then type `s` while holding a button near the
   module. Channels where activity clearly exceeds the background are candidates.
   Pick one with `c` (e.g. `c 50`).
2. **Find the data rate.** These remotes are usually **1 Mbps** (`r 0`) or
   **250 kbps** (`r 2`); try each. Wrong rate = only garbage.
3. **Decode.** Make sure decode is on (`?` shows `decode on`), then `p` to start
   sniffing and press a button. Look for:
   ```
   XN297 OK off=.. alen=5: addr AABBCCDDEE | payload 00 11 22 33 ..
   ```
   `addr` is your remote's address; the `payload` is the command frame. If you
   see nothing, re-check channel/rate and try the other bait variants `a 0`..`a 3`.
4. **Identify the fields.** Press several different buttons (label each with
   `# button name`). Compare the payloads: bytes that stay the same are the
   device ID / header; the byte(s) that change identify the button. A trailing
   byte is often a checksum (try "sum of the preceding bytes, mod 256").
5. **(Optional) clean capture with targeted mode.** Once you know your address,
   put it in `TARGET_ADDR` / `TARGET_ADDR_LEN` near the top of `src/main.cpp`
   (most-significant byte first, exactly as the `addr` line printed), re-flash,
   then `t` + `p`. The radio now matches only your remote, so frames arrive
   byte-aligned and noise-free — ideal for capturing many presses to confirm a
   rolling counter, checksum, etc.

## Adjusting the firmware for your device

Everything you'd normally change is near the top of `src/main.cpp`, under
`ADJUST FOR YOUR REMOTE`:

- **`TARGET_ADDR` / `TARGET_ADDR_LEN`** — your remote's address for targeted mode.
- **`channel` / `rate`** — the defaults applied at boot (also changeable live).
- **`PIN_*`** (further up) — wiring, if you use different GPIOs.

The decoder itself (`XN_SCRAMBLE`, `XN_XOROUT`, `xnCrc`, `tryXn297`) is the
standard XN297 scheme and normally needs no changes. If your device turns out to
be plain nRF24 (no scrambling) or uses a different CRC, that's a bigger change —
the DIY-Multiprotocol-TX-Module project is the best reference for XN297 variants.

## Offline analysis tools

For capturing to a file and analyzing many frames at once, the Python helpers in
[`../../tools/`](../../tools/) parse this firmware's serial output:

- `cap.py` — log the serial stream to a file (with reset / labels).
- `xn297_decode.py` — re-decode raw hex dumps offline, filtering CRC-valid
  repeats (useful when running in raw mode `x`).
- `tframes.py` — decode targeted-mode captures and tally command bytes.

See [`../../captures/README.md`](../../captures/README.md) for examples.

## Safety & scope

Receive-only: this firmware cannot transmit, so it only listens. Sniff and decode
**your own** devices — doing so to equipment you don't own or control may be
illegal where you live. The 2.4 GHz band is shared; be considerate.
