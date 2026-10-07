# Development journal

A chronological log of how this project got from "two remotes that control the
wrong lamps" to a decoded RF protocol — kept as raw material for a future
write-up. Newest entries at the bottom. Dates in the exploration phase are
approximate; the RF-decode dates are exact.

---

## Phase 0 — Exploration & dead-ends *(dates approximate)*

The problem: two identical LED chandeliers and two RF remotes, paired so that one
remote drives both lamps. The wall switch only cuts mains power. Goal: single
smart control point with on/off, dimming, color temperature and night mode, plus
Home Assistant. Several approaches were tried and rejected before landing on
"sniff and replay the RF protocol."

- **BLE control apps (e.g. LampSmart Pro).** Tried on the theory that the lamp
  might be a BLE-advertising mesh light. → **Rejected:** the lamp doesn't respond;
  it isn't BLE. Confirmed the control link is something else (2.4 GHz proprietary).
- **Replace the chandelier's high-voltage LED driver with a Zigbee one.** Would
  give native HA support. → **Rejected:** a drop-in Zigbee driver with matching
  voltage/channel/CCT behavior is hard to source, and it means working on the
  mains side of the fixture — more risk and effort than the problem warrants.
- **Replace only the lamp's RF receiver module.** → **Rejected:** not acceptable —
  still opening the fixture, and no guarantee a compatible receiver exists that
  keeps all features.
- **Solder an ESP onto the physical remotes ("donor" approach).** Drive the
  remote's buttons electrically from an ESP. → **Rejected:** inelegant, consumes
  the remotes, and doesn't scale to clean HA integration.
- **✅ Chosen: sniff and replay the RF protocol.** Listen to what a remote sends,
  decode it, then emulate a remote from an ESP32 + nRF24L01+. No mains work, no
  destroyed hardware, and a clean path to ESPHome/Home Assistant.

---

## Phase 1 — Sniffing & decode

### 2026-10-05 — First contact

- Built the nRF24 promiscuous sniffer on an ESP32-S3-DevKitC-1. Immediately hit
  the native-USB serial trap: no output until building with
  `-DARDUINO_USB_CDC_ON_BOOT=1` and waiting for the USB CDC port to re-enumerate
  after reset.
- Channel scan with the remote held close showed clear activity — the lamp link
  is real and sniffable.

### 2026-10-05 — It's 250 kbps, not 1 Mbps

- Early captures at 1 Mbps produced only smeared, un-decodable bits. Sweeping data
  rates revealed the remote transmits at **250 kbps**. This one setting was the
  difference between noise and clean frames.

### 2026-10-05 — Fixing the XN297 decoder

- The payloads looked XN297-ish (bit-reversed + scrambled). The first decoder
  attempt didn't produce CRC-valid frames. Compared the implementation against the
  authoritative XN297 code in the
  [DIY-Multiprotocol-TX-Module](https://github.com/pascallanger/DIY-Multiprotocol-TX-Module)
  project and found four mismatches: address byte handling, which bytes feed the
  CRC, whether the CRC input is scrambled, and the xorout table. (The constants
  originally suspected — init `0xB5D2`, poly `0x1021`, scramble table — were fine.)
- After matching the reference: **first CRC-valid frames.** 🎉

### 2026-10-05 — Full decode

- Added a "targeted mode" that matches the remote's real 5-byte address instead of
  a bait address, so frames arrive byte-aligned and noise is ignored.
- Captured ON / OFF / NIGHT / DAY. Every frame's checksum and XN297 CRC validated.
  Decoded the payload layout, the `55 2A 75 00` device ID, the command bytes, the
  rolling counter, and the two-burst (phase 00 / phase 01, `+0x40`) behavior. See
  [PROTOCOL.md](PROTOCOL.md).

---

## Phase 2 — Transmit & replay *(in progress)*

### 2026-10-05 — Transmitter added

- Implemented an XN297 encoder (the exact inverse of the verified decoder) and
  `TX`/`CTR` serial commands to emulate a button press: phase-00 burst, 200 ms
  gap, phase-01 burst, hopping the known channels, with correct counter and CRC.
  Each sent frame is printed as hex and validated to be byte-identical to captured
  real frames.
- Open question: **does the lamp enforce the rolling counter?** A first replay
  from across the room produced no reaction — not yet distinguishable between range
  and encoding. Added a hands-free **boot self-test** (arm, send ON, pause, send
  OFF, with RGB-LED status) so the bridge can be tested right next to the lamp
  without a computer attached. Result pending.

### 2026-10-06 — Public documentation

- Structured the repo for public release: `firmware/sniffer/`, reserved
  `firmware/transmitter/`, `captures/`, `tools/`, and wrote README / PROTOCOL /
  HARDWARE / this journal. MIT license.

### 2026-10-06 — Range, redundancy, and a phone remote

- Transmit confirmed working at ~3 m with an occasional dropped frame, so I added
  configurable redundancy: 30 frames per burst, with the whole press repeated up to
  3×. That made it reliable across the room.
- Decoded the color-temperature button: **`07`**, a *relative* "next preset"
  command. Its capture also showed a third burst (phase `02`), which suggests the
  phase byte counts hold-repeats rather than being fixed at two bursts. Relative
  commands are now sent as a single press so a repeat can't skip a preset.
- Added a small Wi-Fi web page to the bridge (big buttons, phone-friendly) so every
  command can be tested standing under the chandelier, without a laptop. The
  credentials live in a gitignored `secrets.h`.

### 2026-10-06 — Second remote, and the Home Assistant integration

- Captured the second chandelier's remote. Same radio address (`AA 55 CC CC CC`),
  but a **different payload ID** (`55 5D 73 00` vs `55 2A 75 00`) — so the two
  lamps *can* be addressed separately over radio. Whether they can actually be
  separated depends on the lamps' pairing, which is still untested (one remote
  currently drives both).
- Built the final firmware as an **ESPHome external component**. The XN297
  transmit code is reused verbatim from the sniffer; only the ESPHome wrapper
  (an on/off light + Night/Day/Next-Temperature buttons per lamp, parameterized by
  lamp ID) is new. Both chandeliers now appear as Home Assistant entities over
  Wi-Fi. Counter persists in flash; the light is optimistic (one-way radio) and
  never transmits on boot.

### 2026-10-07 — Captured brightness, and split out a reusable analyzer

- Decoded the two brightness buttons (up `02`/`42`, down `03`/`43`) on both
  remotes — relative, one step per press, no absolute level. Added to PROTOCOL.md.
- Split the pure analysis part into a standalone **receive-only** firmware
  (`firmware/rx-sniffer/`) with English comments/output and its own guide, so
  anyone with a similar XN297 remote can reuse it to find their own address and
  command bytes without the transmit/Wi-Fi machinery. Expanded the HARDWARE.md
  schematic (full header pinout + connection list) for the write-up.

---

*Next: confirm transmit reception up close, resolve the counter question, then
build the ESPHome transmitter and expose each chandelier to Home Assistant.*
