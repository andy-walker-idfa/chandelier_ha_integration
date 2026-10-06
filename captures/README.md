# Capture logs

Raw and decoded RF logs recorded with the sniffer firmware over USB serial. They
back the values in [../PROTOCOL.md](../PROTOCOL.md). All logs are plain text and
contain only RF protocol bytes and neutral button labels (`# ON`, `# OFF`, …) —
no personal data.

## Line formats

- **Raw promiscuous** (`[ time] chN rR aV: <32 hex bytes>`): everything the radio
  clocked in at an unknown bit offset, including noise.
- **Targeted** (`[ time] chN rR T: <16 hex bytes>` followed by
  `XN297 OK addr ... | payload ...`): frames received by matching the remote's
  real address, plus the firmware's own decode.

Lines beginning with `#` are labels injected from the serial console to mark which
button was being pressed.

## How each log was produced

The sniffer was driven over serial (`r` = data rate, `c` = channel, `t` = targeted
mode, `p` = start/stop, `x` = toggle on-board decode, `a` = bait address,
`# text` = label). The Python helpers in [`../tools/`](../tools/) scripted the
longer sweeps.

| File | What it is |
|------|------------|
| `on_raw.log` | Raw promiscuous capture of the ON button, filter off — the first material used to find alignment and CRC. |
| `sweep_on.log` | ON button swept across channels 48–52 at all three data rates — how 250 kbps was confirmed. |
| `addr_on.log` | ON button at 250 kbps across the four bait-address variants. |
| `chans_on.log` | ON button, 250 kbps, swept over candidate channels to find the hop set. |
| `tchans_on.log` | Targeted mode, full channel sweep 0–83 — the channel-hopping list. |
| `buttons.log` | Raw capture of multiple buttons pressed in sequence. |
| `buttons_t.log` | Targeted-mode capture of ON/OFF/NIGHT/DAY — the clean per-button decode. |
| `verify.log` | Targeted capture confirming the firmware's own `XN297 OK` decode of ON. |
| `live_ctr.log` | A single live ON press used to read the remote's current rolling-counter value. |
| `temp_cycle.log` | Targeted capture of six presses of the TEMP-cycle button (`07`/`47`), showing the phase-02 hold burst. |
| `remote2_probe.log` | **Second remote**, ON ×5 in targeted mode (ch 50). Showed the same radio address as remote 1 with a different payload ID. |
| `remote2_buttons.log` | **Second remote**, OFF / NIGHT / DAY / TEMP ×5 each, labeled `# R2 <button>`. |

> **Before committing a new log:** firmware with Wi-Fi enabled prints the board's
> LAN IP address at boot. Strip that line (and anything else network-related)
> from logs before adding them to the repo.

## Reproducing

```bash
# from the repo root, with the sniffer flashed and on a serial port
python tools/cap.py --reset --secs 20 --log captures/my_capture.log "r 2" "c 50" "t" "p"
# then press a remote button; decode offline:
python tools/tframes.py captures/my_capture.log
```

See each tool's header comment for options.
