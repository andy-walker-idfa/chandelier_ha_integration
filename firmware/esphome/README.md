# ESPHome integration

Final firmware: the ESP32-S3 + nRF24L01+ bridge presented to Home Assistant over
Wi-Fi as an ESPHome device, so both chandeliers appear as native HA entities.

The RF transmit code is an **external component** ([`components/chandelier/`](components/chandelier/))
that reuses the proven XN297 logic from [`../sniffer/`](../sniffer/) verbatim
(same scramble/xorout tables, CRC-16, bit-reversal, burst structure). Only the
ESPHome wrapper (entity classes + codegen) is new.

> **Note:** the ESPHome wrapper was written to ESPHome's conventions but has not
> been compiled in this repo's tooling — your first `esphome compile` is the real
> check. The radio logic inside it is the already-tested transmitter.

## Entities in Home Assistant

| Entity | Type | Action |
|--------|------|--------|
| `light.chandelier_1`, `light.chandelier_2` | Light (on/off) | Sends ON `05` / OFF `09` |
| `button.chandelier_1_night`, `button.chandelier_2_night` | Button | Sends NIGHT `10` |
| `button.chandelier_1_day`, `button.chandelier_2_day` | Button | Sends DAY `11` |
| `button.chandelier_1_next_temperature`, `button.chandelier_2_next_temperature` | Button | Sends TEMP `07` |

**State is one-way / optimistic.** The radio link has no feedback, so each light
shows the *last command sent*, not the lamp's real state — it can drift if the
physical remote is used. Rebooting the bridge never transmits (lights restore as
"off" and the on-boot state apply is suppressed in the component).

**"Next Temperature" is relative.** Each press advances to the next
color-temperature preset in a loop; there is no absolute value to read or set
(see [../../PROTOCOL.md](../../PROTOCOL.md)).

## Setup

1. **Install ESPHome** (if you haven't): `pip install esphome`, or use the ESPHome
   add-on / dashboard in Home Assistant.
2. **Secrets:** copy the template and fill it in. It's gitignored.
   ```bash
   cd firmware/esphome
   cp example.secrets.yaml secrets.yaml
   # edit secrets.yaml: wifi_ssid, wifi_password, ap_password,
   # api_encryption_key (openssl rand -base64 32)
   ```
   OTA reuses `api_encryption_key` (encrypted OTA), so there is no separate OTA
   password to set.
3. The lamp IDs and pins are already set in `chandelier.yaml` (Chandelier 1 =
   `55 2A 75 00`, Chandelier 2 = `55 5D 73 00`; CE=9, CSN=10, SCK=12, MOSI=11,
   MISO=13). Adjust `frames_per_burst` there if you want more redundancy/range.

## Compile & flash

**CLI** (first flash over USB, then OTA afterwards):
```bash
cd firmware/esphome
esphome run chandelier.yaml            # compiles, then offers the USB port or OTA
# or step by step:
esphome compile chandelier.yaml
esphome upload chandelier.yaml --device COM6      # USB; use the device's IP for OTA
esphome logs chandelier.yaml                      # view send logs (command, ID, counter, frames)
```

**Dashboard** (Home Assistant ESPHome add-on or standalone dashboard):
1. Put `chandelier.yaml`, `secrets.yaml` and the `components/` folder in your
   ESPHome config directory (keep the relative layout; `external_components` uses
   `path: components`).
2. Open the device card → **Install** → **Plug into this computer** for the first
   flash (USB), or **Wirelessly** for later updates.

Either way, the first flash must be over USB; subsequent updates can be OTA.

## Add to Home Assistant

With the API enabled (it is, via `api:` + encryption key), HA usually
**auto-discovers** the device: go to **Settings → Devices & Services**, and accept
the discovered "Chandelier Bridge". If not discovered, **Add Integration →
ESPHome**, enter the device's IP (or `chandelier-bridge.local`) and port `6053`,
and paste the `api_encryption_key` from your `secrets.yaml` when prompted.

The six entities above then appear under the Chandelier Bridge device and can be
added to dashboards or the mobile app.

## Debugging

`esphome logs chandelier.yaml` (or the dashboard logs) prints a line per send:
```
[chandelier] TX cmd 05 id 552A7500: 2 bursts x30 frames = 60, counter 0x3C..0x3D
```
The onboard RGB LED also flashes on each send, as on the sniffer firmware.
