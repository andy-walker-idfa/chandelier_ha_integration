#include "chandelier.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include <Arduino.h>
#include <SPI.h>

namespace esphome {
namespace chandelier {

static const char *const TAG = "chandelier";

// ---- nRF24L01+ registers / commands (same as the sniffer firmware) ----
static const uint8_t REG_CONFIG = 0x00, REG_EN_AA = 0x01, REG_SETUP_AW = 0x03,
                     REG_SETUP_RETR = 0x04, REG_RF_CH = 0x05, REG_RF_SETUP = 0x06,
                     REG_STATUS = 0x07, REG_RX_PW_P0 = 0x11, REG_TX_ADDR = 0x10;
static const uint8_t CMD_W_TX_PAYLOAD = 0xA0, CMD_FLUSH_TX = 0xE1;

static SPISettings spi_cfg(4000000, MSBFIRST, SPI_MODE0);

// Channel hop list from PROTOCOL.md. A burst cycles through it.
static const uint8_t CHANNELS[] = {0, 2, 5, 18, 21, 34, 37, 45, 47, 50, 53, 66, 69, 82};
static const uint8_t NCH = sizeof(CHANNELS);

// XN297 scramble + CRC xorout tables (identical to the decoder/sniffer).
static const uint8_t XN_SCRAMBLE[] = {
    0xe3, 0xb1, 0x4b, 0xea, 0x85, 0xbc, 0xe5, 0x66, 0x0d, 0xae, 0x8c, 0x88, 0x12,
    0x69, 0xee, 0x1f, 0xc7, 0x62, 0x97, 0xd5, 0x0b, 0x79, 0xca, 0xcc, 0x1b, 0x5d,
    0x19, 0x10, 0x24, 0xd3, 0xdc, 0x3f, 0x8e, 0xc5, 0x2f, 0xaa, 0x16, 0xf3, 0x95};
static const uint16_t XN_XOROUT[] = {
    0x0000, 0x3448, 0x9BA7, 0x8BBB, 0x85E1, 0x3E8C, 0x451E, 0x18E6, 0x6B24, 0xE7AB,
    0x3828, 0x814B, 0xD461, 0xF494, 0x2503, 0x691D, 0xFE8B, 0x9BA7, 0x8B17, 0x2920,
    0x8B5F, 0x61B1, 0xD391, 0x7401, 0x2138, 0x129F, 0xB3A0, 0x2988, 0x23CA, 0xC0CB,
    0x0C6C, 0xB329, 0xA0A1, 0x0A16, 0xA9D0};

// On-air (scrambled) form of the shared address AA 55 CC CC CC.
static const uint8_t TARGET_AIR[5] = {0x2F, 0x7D, 0x87, 0xBF, 0x2F};

static uint8_t br(uint8_t b) {
  b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
  b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
  b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
  return b;
}
static uint16_t xn_crc(const uint8_t *d, uint8_t n, uint16_t crc = 0xb5d2) {
  for (uint8_t i = 0; i < n; i++) {
    uint8_t b = d[i];
    for (uint8_t j = 0; j < 8; j++) {
      uint8_t bit = (b >> (7 - j)) & 1;
      if (((crc >> 15) & 1) ^ bit) crc = (crc << 1) ^ 0x1021;
      else crc <<= 1;
    }
  }
  return crc;
}

// ---- low-level SPI (same bodies as the sniffer, parameterized by pins) ----
uint8_t ChandelierHub::read_reg_(uint8_t reg) {
  SPI.beginTransaction(spi_cfg);
  digitalWrite(csn_, LOW);
  SPI.transfer(reg & 0x1F);
  uint8_t v = SPI.transfer(0xFF);
  digitalWrite(csn_, HIGH);
  SPI.endTransaction();
  return v;
}
void ChandelierHub::write_reg_(uint8_t reg, uint8_t v) {
  SPI.beginTransaction(spi_cfg);
  digitalWrite(csn_, LOW);
  SPI.transfer(0x20 | (reg & 0x1F));
  SPI.transfer(v);
  digitalWrite(csn_, HIGH);
  SPI.endTransaction();
}
void ChandelierHub::write_reg_buf_(uint8_t reg, const uint8_t *b, uint8_t n) {
  SPI.beginTransaction(spi_cfg);
  digitalWrite(csn_, LOW);
  SPI.transfer(0x20 | (reg & 0x1F));
  for (uint8_t i = 0; i < n; i++) SPI.transfer(b[i]);
  digitalWrite(csn_, HIGH);
  SPI.endTransaction();
}
void ChandelierHub::cmd_(uint8_t c) {
  SPI.beginTransaction(spi_cfg);
  digitalWrite(csn_, LOW);
  SPI.transfer(c);
  digitalWrite(csn_, HIGH);
  SPI.endTransaction();
}
void ChandelierHub::write_tx_payload_(const uint8_t *b, uint8_t n) {
  SPI.beginTransaction(spi_cfg);
  digitalWrite(csn_, LOW);
  SPI.transfer(CMD_W_TX_PAYLOAD);
  for (uint8_t i = 0; i < n; i++) SPI.transfer(b[i]);
  digitalWrite(csn_, HIGH);
  SPI.endTransaction();
}

void ChandelierHub::led_(bool on) {
#if defined(RGB_BUILTIN)
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  rgbLedWrite(RGB_BUILTIN, on ? 12 : 0, on ? 12 : 0, on ? 28 : 0);
#else
  neopixelWrite(RGB_BUILTIN, on ? 12 : 0, on ? 12 : 0, on ? 28 : 0);
#endif
#else
  (void) on;
#endif
}

// Switch the radio to XN297 TX. Preamble 71 0F 55 goes in the hardware TX_ADDR;
// the real (scrambled) address is the first payload bytes. 250 kbps.
void ChandelierHub::radio_tx_setup_() {
  digitalWrite(ce_, LOW);
  write_reg_(REG_CONFIG, 0x02);  // PWR_UP + PRIM_TX, hardware CRC off
  delay(2);
  write_reg_(REG_RF_SETUP, 0x20 | 0x06);  // 250 kbps
  write_reg_(REG_EN_AA, 0x00);
  write_reg_(REG_SETUP_RETR, 0x00);
  write_reg_(REG_SETUP_AW, 0x03);
  const uint8_t pre[5] = {0x55, 0x0F, 0x71, 0x0C, 0x00};
  write_reg_buf_(REG_TX_ADDR, pre, 5);
  cmd_(CMD_FLUSH_TX);
  write_reg_(REG_STATUS, 0x70);
}

// One frame: on-air address + 8 payload bytes + CRC16, built exactly inverse to
// the receive decoder (bit-reverse + scramble + same xn_crc / XN_XOROUT).
void ChandelierHub::tx_frame_(uint8_t ch, const uint8_t payload8[8]) {
  uint8_t buf[15];
  for (uint8_t i = 0; i < 5; i++) buf[i] = TARGET_AIR[i];
  for (uint8_t i = 0; i < 8; i++) buf[5 + i] = br(payload8[i]) ^ XN_SCRAMBLE[5 + i];
  uint16_t crc = xn_crc(buf, 13) ^ XN_XOROUT[5 - 3 + 8];
  buf[13] = crc >> 8;
  buf[14] = crc & 0xFF;

  digitalWrite(ce_, LOW);
  write_reg_(REG_RF_CH, ch);
  cmd_(CMD_FLUSH_TX);
  write_reg_(REG_STATUS, 0x70);
  write_tx_payload_(buf, 15);
  digitalWrite(ce_, HIGH);
  delayMicroseconds(20);
  digitalWrite(ce_, LOW);
  uint32_t t0 = micros();
  while (!(read_reg_(REG_STATUS) & 0x20) && micros() - t0 < 3000) {
  }
  write_reg_(REG_STATUS, 0x70);
}

// One burst: frames_ identical frames cycling the channel list; counter constant.
uint16_t ChandelierHub::tx_burst_(uint8_t phase, uint8_t command, uint8_t ctr, const uint8_t id[4]) {
  uint8_t pl[8] = {phase, id[0], id[1], id[2], id[3], command, ctr, 0};
  uint16_t s = 0;
  for (uint8_t i = 0; i < 7; i++) s += pl[i];
  pl[7] = s & 0xFF;  // checksum = sum(bytes 0..6) mod 256

  led_(true);
  for (uint8_t i = 0; i < frames_; i++) {
    tx_frame_(CHANNELS[i % NCH], pl);
    delayMicroseconds(800);
    App.feed_wdt();
  }
  led_(false);
  return frames_;
}

void ChandelierHub::send(uint8_t id0, uint8_t id1, uint8_t id2, uint8_t id3, uint8_t command) {
  const uint8_t id[4] = {id0, id1, id2, id3};
  uint8_t ctr_start = counter_ & 0xFF;
  uint16_t total = 0;

  radio_tx_setup_();
  total += tx_burst_(0x00, command, counter_ & 0xFF, id);
  counter_++;
  delay(200);
  total += tx_burst_(0x01, command | 0x40, counter_ & 0xFF, id);
  counter_++;

  pref_.save(&counter_);
  global_preferences->sync();

  digitalWrite(ce_, LOW);
  write_reg_(REG_CONFIG, 0x00);  // power down between presses

  ESP_LOGI(TAG, "TX cmd %02X id %02X%02X%02X%02X: 2 bursts x%u frames = %u, counter 0x%02X..0x%02X",
           command, id0, id1, id2, id3, frames_, total, ctr_start, (uint8_t) ((counter_ - 1) & 0xFF));
}

void ChandelierHub::setup() {
  pinMode(ce_, OUTPUT);
  pinMode(csn_, OUTPUT);
  digitalWrite(ce_, LOW);
  digitalWrite(csn_, HIGH);
  SPI.begin(sck_, miso_, mosi_, csn_);
  delay(5);

  // Base state: powered down, no auto-ack, 32-byte payload width.
  write_reg_(REG_CONFIG, 0x00);
  write_reg_(REG_EN_AA, 0x00);
  write_reg_(REG_RX_PW_P0, 32);

  pref_ = global_preferences->make_preference<uint32_t>(fnv1_hash("chandelier_counter"));
  if (!pref_.load(&counter_)) counter_ = 0;
  led_(false);
  ESP_LOGCONFIG(TAG, "Chandelier hub ready: CE=%u CSN=%u SCK=%u MOSI=%u MISO=%u, %u frames/burst, counter 0x%02X",
                ce_, csn_, sck_, mosi_, miso_, frames_, (uint8_t) (counter_ & 0xFF));
}

// ---------------- light ----------------
light::LightTraits ChandelierLight::get_traits() {
  auto traits = light::LightTraits();
  traits.set_supported_color_modes({light::ColorMode::ON_OFF});
  return traits;
}

void ChandelierLight::setup() {
  // Become ready only after setup/restore has been applied, so the restored
  // initial state doesn't transmit a command on boot.
  this->set_timeout(250, [this]() { this->ready_ = true; });
}

void ChandelierLight::write_state(light::LightState *state) {
  bool on = false;
  state->current_values_as_binary(&on);
  if (!ready_ || hub_ == nullptr) return;  // ignore boot/restore apply
  hub_->send(id_[0], id_[1], id_[2], id_[3], on ? CMD_ON : CMD_OFF);
}

}  // namespace chandelier
}  // namespace esphome
