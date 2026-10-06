# Transmitter firmware — *in progress*

This directory is reserved for the **final ESPHome-based transmitter** that will
expose each chandelier to Home Assistant as a light (on/off, dimming, color
temperature, night mode).

**Status:** not started. Transmit and replay are currently prototyped inside the
sniffer firmware ([`../sniffer/`](../sniffer/)) via its `TX` / `CTR` serial
commands and boot self-test, which is where the XN297 encoder is being validated
against the real lamp.

Planned once replay is confirmed and the rolling-counter question
([PROTOCOL.md → Open questions](../../PROTOCOL.md#open-questions)) is resolved:

- ESPHome external component that emits the XN297 burst for a given command.
- Home Assistant light entities per chandelier.
- Rolling-counter persistence (if the lamp turns out to enforce it).
