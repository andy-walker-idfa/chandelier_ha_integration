# Chandelier Bridge — Home Assistant control for an XN297L RF LED chandelier

Reverse-engineering the 2.4 GHz radio protocol of a cheap LED chandelier's remote
(XN297L-based) so the chandelier can be controlled from **Home Assistant** via an
**ESP32 + nRF24L01+** bridge, replacing the handheld remotes and the wall switch
while keeping dimming, color temperature and night mode.

![status](https://img.shields.io/badge/protocol-decoded-brightgreen)
![status](https://img.shields.io/badge/transmitter-in%20progress-yellow)
![status](https://img.shields.io/badge/ESPHome%20%2F%20HA-planned-lightgrey)
![license](https://img.shields.io/badge/license-MIT-blue)

> **Have a similar lamp?** If your chandelier remote looks like this protocol but
> the addresses or command bytes differ, please open an issue or PR with your
> capture — collecting protocol variants is a goal of this repo. See
> [CONTRIBUTING](#contributing).

---

## Why

Two identical LED chandeliers, two handheld RF remotes. The remotes got paired
such that **one remote now controls both chandeliers** — they can no longer be
operated independently. On top of that:

- The **wall switch cuts mains power**, so the only "smart" state is lost whenever
  someone flips it, and you can't dim or change color temperature from the wall.
- There is **no app or hub** — control is RF-remote-only.
- The goal is **single-point control with the full feature set** (on/off, dimming,
  color temperature, night mode) **and Home Assistant integration**.

Rather than replace the chandelier's mains-side driver or solder onto the physical
remotes, this project **sniffs and replays the remote's own RF protocol**, so an
ESP32 bridge can emulate a remote and expose each chandelier to Home Assistant.
(See [JOURNAL.md](JOURNAL.md) for the dead-ends that led here.)

## Status

| Part | State | Notes |
|------|-------|-------|
| RF protocol decode | ✅ Done | Two remotes decoded; address, payload, commands, checksum, CRC — [PROTOCOL.md](PROTOCOL.md) |
| Sniffer firmware | ✅ Working | Receives and decodes live frames, prints `XN297 OK` lines |
| Transmitter (replay) | ✅ Working | Emulates a remote; all commands confirmed on the lamp |
| ESPHome / Home Assistant | ✅ Working | Both chandeliers as HA entities (Chandelier 1 tested) — [firmware/esphome/](firmware/esphome/) |
| Rolling-counter enforcement | ❓ Unknown | Appears loose/absent; see [PROTOCOL.md](PROTOCOL.md#open-questions) |
| Independent control of both lamps | ❓ Untested | Remotes have distinct IDs, but lamp pairing not yet tested — [PROTOCOL.md](PROTOCOL.md#open-questions) |

## Hardware at a glance

- **ESP32-S3-DevKitC-1** (native USB)
- **nRF24L01+** module (PCB-antenna version is fine for bench work)
- **10–100 µF capacitor** across the nRF24's VCC/GND, right at the module
- A few jumper wires

Wiring (ESP32-S3):

| nRF24L01+ | ESP32-S3 |
|-----------|----------|
| CE   | GPIO9  |
| CSN  | GPIO10 |
| MOSI | GPIO11 |
| SCK  | GPIO12 |
| MISO | GPIO13 |
| VCC  | 3V3 |
| GND  | GND |
| IRQ  | (not connected) |

Full bill of materials, caveats and the classic-ESP32 pinout are in
[HARDWARE.md](HARDWARE.md).

## Quick start

1. **Build & flash the analyzer** (PlatformIO):
   ```bash
   cd firmware/sniffer
   pio run -t upload
   ```
2. **Open the serial monitor** at `115200` baud (line ending: newline). On the
   ESP32-S3 native USB you must build with `-DARDUINO_USB_CDC_ON_BOOT=1` or the
   port stays silent — it's already set in `platformio.ini`.
3. You should see `nRF24 found.` and a command menu.
4. **Watch live frames** from the remote:
   ```
   r 2      # 250 kbps
   c 50     # a channel the remote uses
   p        # start sniffing
   ```
   Press a button on the remote; valid frames print as
   `XN297 OK off=.. alen=5: addr AABBCCDDEE | payload ...`.

## Analyzing your own remote

If you have a similar XN297-based remote, the receive-only
[**`firmware/sniffer/`**](firmware/sniffer/) is a reusable analyzer with its
own step-by-step guide — flash it, press your buttons, and read off your
device's address, IDs and command bytes. That's the recommended starting point.

The larger [`firmware/dev-bridge/`](firmware/dev-bridge/) is this project's combined
dev firmware (it also transmits, serves a Wi-Fi test page, etc.); its full serial
command set is documented in the header of
[`firmware/dev-bridge/src/main.cpp`](firmware/dev-bridge/src/main.cpp).

## Repository map

| Path | What's inside |
|------|---------------|
| [README.md](README.md) | This overview |
| [PROTOCOL.md](PROTOCOL.md) | The full decoded RF protocol |
| [HARDWARE.md](HARDWARE.md) | Bill of materials and wiring |
| [JOURNAL.md](JOURNAL.md) | Chronological development log / article notes |
| [`firmware/sniffer/`](firmware/sniffer/) | **Receive-only analyzer** — the reusable sniffer/decoder with its own guide ([README](firmware/sniffer/README.md)) |
| [`firmware/dev-bridge/`](firmware/dev-bridge/) | Combined dev firmware: sniff + replay + Wi-Fi test page (PlatformIO, ESP32-S3) |
| [`firmware/esphome/`](firmware/esphome/) | Final ESPHome integration — both chandeliers as Home Assistant entities |
| [`captures/`](captures/) | Raw and decoded RF capture logs ([how they were made](captures/README.md)) |
| [`tools/`](tools/) | Python helpers for capturing and offline decoding |

## Safety & legal

- ⚠️ **Mains voltage.** The chandelier's LED driver runs on mains. This project
  deliberately **does not touch the high-voltage side** — it only emulates the
  wireless remote. Any work inside the fixture is at your own risk and may require
  a qualified electrician depending on your jurisdiction.
- ⚠️ **RF transmission.** The nRF24L01+ operates in the 2.4 GHz ISM band. Keep
  transmit power and duty cycle reasonable and within your local regulations.
- ⚖️ **Only your own devices.** Sniffing and replaying a radio protocol is done
  here strictly on **hardware the author owns**. Doing so against devices you do
  not own or control may be illegal where you live. This repository is for
  interoperability and personal automation of your own equipment.

## License & contributing

MIT — see [LICENSE](LICENSE). Protocol variants and improvements welcome; see
[CONTRIBUTING.md](CONTRIBUTING.md).
