# Contributing

Contributions are welcome — bug fixes, firmware improvements, and especially
**protocol variants from similar lamps**.

## Got a similar chandelier/remote?

Many cheap 2.4 GHz LED lamps use XN297/XN297L radios with the same encoding but
different addresses, device IDs or command bytes. If yours looks like
[this protocol](PROTOCOL.md) but the values differ, please share a capture:

1. Flash the sniffer (`firmware/sniffer/`) and put it in targeted or raw mode.
2. Capture each button a few times (see [`captures/README.md`](captures/README.md)).
3. Open an issue or PR including:
   - the lamp/remote model (and a photo if handy),
   - the radio params you found (data rate, channels, address),
   - the decoded payload layout and command bytes,
   - a short raw capture log.

Collecting these variants is an explicit goal of the repo — it helps everyone
with one of these lamps.

## Code

- Firmware is PlatformIO + Arduino (ESP32). Keep the sniffer's existing serial
  command style and comment conventions.
- Python helpers target the stdlib + `pyserial`; keep them dependency-light.
- **No personal data** in commits: no real names, Wi-Fi SSIDs, MAC addresses,
  network details, home addresses or account info — in code, logs or screenshots.

## License

By contributing you agree your contributions are licensed under the repository's
[MIT License](LICENSE).
