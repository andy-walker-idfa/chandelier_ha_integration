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

---

*Next: confirm transmit reception up close, resolve the counter question, then
build the ESPHome transmitter and expose each chandelier to Home Assistant.*
