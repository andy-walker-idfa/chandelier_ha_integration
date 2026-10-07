# Hardware

The bridge is deliberately minimal: an ESP32 dev board, an nRF24L01+ radio, and
one capacitor. Nothing touches the chandelier's mains side.

## Bill of materials

| Qty | Part | Notes |
|-----|------|-------|
| 1 | **ESP32-S3-DevKitC-1** | Native USB; any ESP32-S3 dev board works with pin changes. A classic ESP32 DevKit also works — see [alternate pinout](#classic-esp32-pinout). |
| 1 | **nRF24L01+ module** | PCB-antenna version is fine for bench work; the external-antenna (nRF24L01+PA+LNA) version gives more range for the final install. Must be a genuine or good-quality clone — see [caveats](#nrf24-clonepower-caveats). |
| 1 | **10–100 µF capacitor** | Electrolytic or ceramic, placed **directly across the nRF24's VCC and GND pins** at the module. Not optional. |
| — | **Jumper wires** | 7 female-female jumpers. |

## Wiring (ESP32-S3-DevKitC-1)

| nRF24L01+ pin | ESP32-S3 pin |
|---------------|--------------|
| CE   | GPIO9  |
| CSN  | GPIO10 |
| MOSI | GPIO11 |
| SCK  | GPIO12 |
| MISO | GPIO13 |
| VCC  | 3V3 |
| GND  | GND |
| IRQ  | *(leave unconnected)* |
| — | **10–100 µF capacitor across VCC ↔ GND, at the module** |

### Schematic

The nRF24L01+ module has a 2×4 pin header, viewed from the top (antenna away
from you). Only 7 of the 8 pins are used — IRQ is left unconnected:

```
            nRF24L01+ header (top view)
          ┌───────────────────────────┐
     GND  │ ● GND        ● VCC         │  3V3  (+ 10–100 µF cap to GND here)
     CSN  │ ● CE         ● CSN         │  → see connections
     SCK  │ ● SCK        ● MOSI        │
    MISO  │ ● MISO       ● IRQ   (n/c) │
          └───────────────────────────┘
```

Connections, in signal order:

```
  ESP32-S3 3V3  ──┬──────────────  nRF24 VCC
                  │
              [ 10–100 µF ]         electrolytic or ceramic, soldered/pushed
                  │                 directly across the module's VCC and GND
  ESP32-S3 GND  ──┴──────────────  nRF24 GND
  ESP32-S3 GPIO9  ───────────────  nRF24 CE     (chip enable)
  ESP32-S3 GPIO10 ───────────────  nRF24 CSN    (SPI chip select)
  ESP32-S3 GPIO12 ───────────────  nRF24 SCK    (SPI clock)
  ESP32-S3 GPIO11 ───────────────  nRF24 MOSI   (ESP32 → nRF24)
  ESP32-S3 GPIO13 ───────────────  nRF24 MISO   (nRF24 → ESP32)
  nRF24 IRQ  ── not connected
```

All signals are 3.3 V logic (the ESP32-S3 drives them directly; the nRF24 is a
3.3 V part). VCC must be **3.3 V, not 5 V**.

> The onboard RGB LED on `RGB_BUILTIN` (GPIO48 on the DevKitC-1) is used by the
> firmware's boot self-test as a status indicator — no wiring needed.

### Classic ESP32 pinout

The firmware auto-selects pins by target. On a classic ESP32 DevKit (VSPI):

| nRF24L01+ | ESP32 |
|-----------|-------|
| CE   | GPIO4  |
| CSN  | GPIO5  |
| SCK  | GPIO18 |
| MOSI | GPIO23 |
| MISO | GPIO19 |

## Gotchas

### Native-USB serial silence

The ESP32-S3-DevKitC-1 uses the chip's **native USB** (no separate UART chip). USB
CDC serial output only works if the firmware is built with:

```ini
build_flags = -DARDUINO_USB_CDC_ON_BOOT=1
```

This is already set in [`firmware/sniffer/platformio.ini`](firmware/sniffer/platformio.ini).
Without it the serial monitor stays completely empty even though the board runs.

A second symptom of native USB: the port **disappears and re-enumerates on every
reset**, so the startup banner can be printed before any monitor reconnects. The
firmware waits up to 5 s for the host to attach before printing, which mostly
hides this — but if you reset and see nothing, reconnect the monitor and press the
board's reset button once more.

If upload fails with *"port busy"/"access denied"*, close any open serial monitor
first — it holds the port.

### nRF24 clone / power caveats

- **Decoupling is mandatory.** nRF24L01+ modules (especially cheap clones) draw
  current spikes that the dev board's 3V3 rail can't absorb cleanly. Without the
  capacitor across VCC/GND you get intermittent SPI failures and "radio not found"
  errors. Use the largest cap that fits (10–100 µF).
- **3.3 V only.** The nRF24 is **not** 5 V tolerant on VCC. Logic pins are driven
  by the ESP32 at 3.3 V, which is correct.
- **Clone quality varies.** Some clones mislabel the chip or have weak PAs. If you
  see no traffic at all, try another module before suspecting the code.
- **Promiscuous mode is a hack.** This project uses the undocumented 2-byte-address
  trick to sniff; reception is noisy by nature. The "targeted mode" (matching the
  real 5-byte address) is far cleaner once the address is known.

## Photos

*(placeholder — add wiring and final-install photos here)*

- `docs/images/wiring.jpg` — breadboard wiring of ESP32-S3 + nRF24L01+
- `docs/images/install.jpg` — final bridge near the chandelier
