#pragma once
//
// ESPHome external component for the XN297L LED chandelier bridge.
//
// The radio logic here is a direct port of the proven transmit code from
// firmware/dev-bridge/src/main.cpp (XN297 encode: bit-reverse + scramble + CRC-16,
// nRF24L01+ in TX mode, channel hopping). It is NOT reimplemented — the byte
// layout, scramble/xorout tables, CRC and burst structure are identical.
//
// One ChandelierHub owns the radio. The transmit routine is parameterized by the
// 4-byte lamp ID, so the same code drives both chandeliers (they share the radio
// address AA 55 CC CC CC and differ only by the ID bytes in the payload).
//
// State is COMMAND-TRACKED and one-way: the radio link has no feedback, so the
// light entity is optimistic (HA shows what we last commanded, which can drift if
// someone uses the physical remote). The temperature-cycle button is RELATIVE —
// each press advances to the next preset and there is no knowable absolute state.

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"

namespace esphome {
namespace chandelier {

// Button command bytes (phase 00); phase 01 adds 0x40. From PROTOCOL.md.
static const uint8_t CMD_ON = 0x05, CMD_OFF = 0x09, CMD_NIGHT = 0x10, CMD_DAY = 0x11, CMD_TEMP = 0x07;

class ChandelierHub : public Component {
 public:
  void set_ce_pin(uint8_t p) { ce_ = p; }
  void set_cs_pin(uint8_t p) { csn_ = p; }
  void set_sck_pin(uint8_t p) { sck_ = p; }
  void set_mosi_pin(uint8_t p) { mosi_ = p; }
  void set_miso_pin(uint8_t p) { miso_ = p; }
  void set_frames_per_burst(uint8_t n) { frames_ = n; }

  void setup() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // Emulate one full button press for the given lamp ID: a phase-00 burst, then
  // ~200 ms later a phase-01 burst (command | 0x40). The rolling counter is held
  // constant within a burst and incremented once per burst (matching the real
  // remote and the verified sniffer firmware), so every frame stays checksum- and
  // CRC-valid. Blocks for ~260 ms. Persists the counter to flash.
  void send(uint8_t id0, uint8_t id1, uint8_t id2, uint8_t id3, uint8_t command);

 protected:
  uint8_t read_reg_(uint8_t reg);
  void write_reg_(uint8_t reg, uint8_t v);
  void write_reg_buf_(uint8_t reg, const uint8_t *b, uint8_t n);
  void cmd_(uint8_t c);
  void write_tx_payload_(const uint8_t *b, uint8_t n);
  void radio_tx_setup_();
  void tx_frame_(uint8_t ch, const uint8_t payload8[8]);
  uint16_t tx_burst_(uint8_t phase, uint8_t command, uint8_t ctr, const uint8_t id[4]);
  void led_(bool on);

  uint8_t ce_{9}, csn_{10}, sck_{12}, mosi_{11}, miso_{13}, frames_{30};
  uint32_t counter_{0};
  ESPPreferenceObject pref_;
};

// On/off-only light. Optimistic: write_state transmits the ON or OFF command.
// Boot/restore does NOT transmit (guarded by ready_), so rebooting the bridge
// never flips the lamp.
class ChandelierLight : public Component, public light::LightOutput {
 public:
  void set_hub(ChandelierHub *hub) { hub_ = hub; }
  void set_lamp_id(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    id_[0] = a; id_[1] = b; id_[2] = c; id_[3] = d;
  }
  void setup() override;
  light::LightTraits get_traits() override;
  void write_state(light::LightState *state) override;

 protected:
  ChandelierHub *hub_{nullptr};
  uint8_t id_[4]{0, 0, 0, 0};
  bool ready_{false};
};

}  // namespace chandelier
}  // namespace esphome
