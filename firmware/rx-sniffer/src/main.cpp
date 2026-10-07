#include <Arduino.h>
/*
  XN297 remote analyzer — receive only (ESP32 + nRF24L01+)
  ========================================================

  A self-contained tool for reverse-engineering cheap 2.4 GHz remotes that use
  Nordic nRF24L01 / XN297 / XN297L radios (many LED lamps, ceiling fans and toys).
  It puts the nRF24 into a promiscuous receive mode, slides an XN297 decoder over
  every bit offset, de-scrambles, and prints only frames whose XN297 CRC-16 is
  valid — which is a near-certain sign the decode is correct and byte-aligned.

  There is NO transmit here on purpose: this firmware only listens, so it is safe
  to use for analysis and cannot command anything. (The full project also has a
  transmitter; see the repository README.)

  ---- Wiring (ESP32-S3-DevKitC-1) --------------------------------------------
    nRF24L01+ VCC  -> 3V3   (+ a 10-100 uF capacitor across VCC/GND AT the module)
    nRF24L01+ GND  -> GND
    nRF24L01+ CE   -> GPIO9
    nRF24L01+ CSN  -> GPIO10
    nRF24L01+ SCK  -> GPIO12
    nRF24L01+ MOSI -> GPIO11
    nRF24L01+ MISO -> GPIO13
    nRF24L01+ IRQ  -> not connected
  Classic ESP32 pins are selected automatically below (CE4/CSN5/SCK18/MOSI23/MISO19).
  The decoupling capacitor is NOT optional with cheap modules.

  ---- Serial console (115200 baud, newline line ending) ----------------------
    b        background channel scan (do NOT touch the remote) ~10 s
    s        activity scan WHILE you press the remote button    ~10 s
    c N      set channel N (0..125);     2.4 GHz freq = 2400 + N MHz
    r N      set data rate: 0 = 1 Mbps, 1 = 2 Mbps, 2 = 250 kbps
    a N      bait-address variant 0..3 (promiscuous capture, see notes)
    f        toggle "show repeats only" filter for raw dumps
    x        toggle the XN297 decoder (off = print raw 32-byte hex)
    t        toggle targeted mode (match ONE known address; see TARGET_ADDR)
    p        start / stop sniffing
    # text   print a label line into the log (e.g. "# button A")
    ?        help / current settings

  ---- How to analyze YOUR remote (quick version) -----------------------------
    1) 'b' with the remote idle, then 's' while holding a button: note the busy
       channels. Set one with 'c'. Try each rate with 'r' (0/1/2); most of these
       remotes are 1 Mbps or 250 kbps.
    2) Make sure the decoder is on ('x' shows "XN297 ... on"), then 'p' and press
       the button. Watch for lines like:
         XN297 OK off=.. alen=5: addr AABBCCDDEE | payload 00 11 22 ..
       The 'addr' is your remote's address; the payload bytes are the command.
    3) Press several buttons; the byte(s) that change between them are the command
       field. See README.md in this folder for the full walkthrough and for how to
       switch to the cleaner targeted mode once you know your address.
*/

#include <SPI.h>

#if defined(CONFIG_IDF_TARGET_ESP32S3)
  #define PIN_CE   9
  #define PIN_CSN  10
  #define PIN_MOSI 11
  #define PIN_SCK  12
  #define PIN_MISO 13
#else  // classic ESP32 DevKit (VSPI)
  #define PIN_CE   4
  #define PIN_CSN  5
  #define PIN_SCK  18
  #define PIN_MISO 19
  #define PIN_MOSI 23
#endif

#define REG_CONFIG      0x00
#define REG_EN_AA       0x01
#define REG_EN_RXADDR   0x02
#define REG_SETUP_AW    0x03
#define REG_SETUP_RETR  0x04
#define REG_RF_CH       0x05
#define REG_RF_SETUP    0x06
#define REG_STATUS      0x07
#define REG_RPD         0x09
#define REG_RX_ADDR_P0  0x0A
#define REG_RX_PW_P0    0x11
#define REG_FIFO_STATUS 0x17
#define REG_DYNPD       0x1C
#define REG_FEATURE     0x1D

#define CMD_R_RX_PAYLOAD 0x61
#define CMD_FLUSH_RX     0xE2

SPISettings spiCfg(4000000, MSBFIRST, SPI_MODE0);

// ===========================================================================
// ADJUST FOR YOUR REMOTE
// ===========================================================================
// Bait addresses for promiscuous capture. The nRF24 is told to match a very
// short, common bit pattern so it triggers on almost any traffic; the decoder
// then recovers the real frame by sliding. If you see nothing, try 'a 0'..'a 3'.
const uint8_t ADDR[4][2] = {{0xAA, 0x00}, {0x55, 0x00}, {0x00, 0xAA}, {0x00, 0x55}};

// Targeted mode ('t'): once you have discovered your remote's LOGICAL address
// from an "XN297 OK ... addr ..." line, put it here (most significant byte
// first, exactly as printed) and set the length. Targeted mode then receives
// only that remote, byte-aligned and noise-free. The on-air (scrambled) form is
// computed for you, so you only edit these two lines.
uint8_t TARGET_ADDR[5] = {0xAA, 0x55, 0xCC, 0xCC, 0xCC};
uint8_t TARGET_ADDR_LEN = 5;  // 3, 4 or 5

// Channel to start on (change with 'c' at runtime too).
uint8_t channel = 50;
// Data rate: 0 = 1 Mbps, 1 = 2 Mbps, 2 = 250 kbps.
uint8_t rate = 2;
// ===========================================================================

uint8_t addrVariant = 0;
bool filterOn = true;
bool sniffing = false;
bool targeted = false;
bool xn297Decode = true;

uint16_t hits[126];
uint16_t base[126];
bool haveBase = false;

// ---------- low-level SPI ----------
uint8_t readReg(uint8_t reg) {
  SPI.beginTransaction(spiCfg);
  digitalWrite(PIN_CSN, LOW);
  SPI.transfer(reg & 0x1F);
  uint8_t v = SPI.transfer(0xFF);
  digitalWrite(PIN_CSN, HIGH);
  SPI.endTransaction();
  return v;
}
void writeReg(uint8_t reg, uint8_t v) {
  SPI.beginTransaction(spiCfg);
  digitalWrite(PIN_CSN, LOW);
  SPI.transfer(0x20 | (reg & 0x1F));
  SPI.transfer(v);
  digitalWrite(PIN_CSN, HIGH);
  SPI.endTransaction();
}
void writeRegBuf(uint8_t reg, const uint8_t *b, uint8_t n) {
  SPI.beginTransaction(spiCfg);
  digitalWrite(PIN_CSN, LOW);
  SPI.transfer(0x20 | (reg & 0x1F));
  for (uint8_t i = 0; i < n; i++) SPI.transfer(b[i]);
  digitalWrite(PIN_CSN, HIGH);
  SPI.endTransaction();
}
void command(uint8_t c) {
  SPI.beginTransaction(spiCfg);
  digitalWrite(PIN_CSN, LOW);
  SPI.transfer(c);
  digitalWrite(PIN_CSN, HIGH);
  SPI.endTransaction();
}
void readPayload(uint8_t *b, uint8_t n) {
  SPI.beginTransaction(spiCfg);
  digitalWrite(PIN_CSN, LOW);
  SPI.transfer(CMD_R_RX_PAYLOAD);
  for (uint8_t i = 0; i < n; i++) b[i] = SPI.transfer(0xFF);
  digitalWrite(PIN_CSN, HIGH);
  SPI.endTransaction();
}

// ---------- XN297 decoder (standard tables; shared across XN297 devices) ----------
static const uint8_t XN_SCRAMBLE[] = {
  0xe3,0xb1,0x4b,0xea,0x85,0xbc,0xe5,0x66,0x0d,0xae,0x8c,0x88,0x12,0x69,0xee,0x1f,
  0xc7,0x62,0x97,0xd5,0x0b,0x79,0xca,0xcc,0x1b,0x5d,0x19,0x10,0x24,0xd3,0xdc,0x3f,
  0x8e,0xc5,0x2f,0xaa,0x16,0xf3,0x95 };
// CRC xorout, indexed by (address length - 3 + payload length).
static const uint16_t XN_XOROUT[] = {
  0x0000,0x3448,0x9BA7,0x8BBB,0x85E1,0x3E8C,0x451E,0x18E6,0x6B24,0xE7AB,0x3828,
  0x814B,0xD461,0xF494,0x2503,0x691D,0xFE8B,0x9BA7,0x8B17,0x2920,0x8B5F,0x61B1,
  0xD391,0x7401,0x2138,0x129F,0xB3A0,0x2988,0x23CA,0xC0CB,0x0C6C,0xB329,0xA0A1,
  0x0A16,0xA9D0 };

static uint8_t br(uint8_t b) {  // reverse the 8 bits of a byte
  b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
  b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
  b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
  return b;
}
static uint16_t xnCrc(const uint8_t *d, uint8_t n, uint16_t crc = 0xb5d2) {
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

uint32_t lastHash = 0, lastTime = 0;

// Slide an XN297 decoder over a raw 32-byte buffer at every bit offset, trying
// each plausible address and payload length, and print frames with a valid CRC.
// Returns a hash of the first valid frame (for de-duplication) or 0.
uint32_t tryXn297(const uint8_t *raw) {
  uint8_t bits[256];
  for (int i = 0; i < 256; i++) bits[i] = (raw[i >> 3] >> (7 - (i & 7))) & 1;

  for (int off = 0; off <= 256 - 56; off++) {
    uint8_t seg[30];
    int avail = (256 - off) / 8;
    if (avail > 30) avail = 30;
    for (int k = 0; k < avail; k++) {
      uint8_t v = 0;
      for (int b = 0; b < 8; b++) v = (v << 1) | bits[off + k * 8 + b];
      seg[k] = v;
    }
    for (int alen = 3; alen <= 5; alen++) {
      for (int plen = 1; plen <= 16; plen++) {
        int total = alen + plen;
        if (total + 2 > avail) continue;
        // De-scramble. Address: reversed byte order, XOR scramble. Payload:
        // XOR scramble then bit-reverse. CRC is over the scrambled on-air bytes.
        uint8_t dew[22];
        for (int i = 0; i < alen; i++) dew[i] = seg[alen - 1 - i] ^ XN_SCRAMBLE[alen - 1 - i];
        for (int i = alen; i < total; i++) dew[i] = br(seg[i] ^ XN_SCRAMBLE[i]);
        uint16_t crc = xnCrc(seg, total) ^ XN_XOROUT[alen - 3 + plen];
        uint16_t tx = (uint16_t)(seg[total] << 8) | seg[total + 1];
        if (crc == tx) {
          Serial.printf("[%8lu] XN297 OK off=%d alen=%d: addr ", millis(), off, alen);
          for (int i = 0; i < alen; i++) Serial.printf("%02X", dew[i]);
          Serial.print(" | payload ");
          for (int i = alen; i < total; i++) Serial.printf("%02X ", dew[i]);
          Serial.println();
          uint32_t h = 2166136261u;
          for (int i = alen; i < total; i++) { h ^= dew[i]; h *= 16777619u; }
          return h ? h : 1;
        }
      }
    }
  }
  return 0;
}

// ---------- radio configuration ----------
void setRate(uint8_t r) {
  uint8_t v = (r == 1) ? 0x08 : (r == 2) ? 0x20 : 0x00;
  writeReg(REG_RF_SETUP, v | 0x06);
}

// Load the receive address. Targeted mode computes the on-air (scrambled) form
// of TARGET_ADDR; bait mode uses the undocumented 2-byte address trick.
void loadAddress() {
  if (targeted) {
    uint8_t n = TARGET_ADDR_LEN;
    uint8_t air[5], reg[5];
    for (uint8_t j = 0; j < n; j++) air[j] = TARGET_ADDR[n - 1 - j] ^ XN_SCRAMBLE[j];
    for (uint8_t j = 0; j < n; j++) reg[j] = air[n - 1 - j];  // nRF wants LSByte first
    writeReg(REG_SETUP_AW, n - 2);
    writeRegBuf(REG_RX_ADDR_P0, reg, n);
  } else {
    writeReg(REG_SETUP_AW, 0x00);  // 2-byte "bait" address (undocumented mode)
    writeRegBuf(REG_RX_ADDR_P0, ADDR[addrVariant], 2);
  }
}

void applySniffConfig() {
  digitalWrite(PIN_CE, LOW);
  loadAddress();
  setRate(rate);
  writeReg(REG_RF_CH, channel);
  command(CMD_FLUSH_RX);
  writeReg(REG_STATUS, 0x70);
  if (sniffing) digitalWrite(PIN_CE, HIGH);
}

bool radioInit() {
  writeReg(REG_SETUP_AW, 0x03);
  if (readReg(REG_SETUP_AW) != 0x03) return false;  // radio not responding
  writeReg(REG_CONFIG, 0x00);
  writeReg(REG_EN_AA, 0x00);
  writeReg(REG_EN_RXADDR, 0x01);
  writeReg(REG_SETUP_RETR, 0x00);
  writeReg(REG_DYNPD, 0x00);
  writeReg(REG_FEATURE, 0x00);
  writeReg(REG_RX_PW_P0, 32);
  writeReg(REG_CONFIG, 0x03);  // PWR_UP + PRIM_RX, hardware CRC off
  delay(5);
  applySniffConfig();
  return true;
}

// ---------- channel scanner ----------
void doScan(uint16_t *out, uint16_t sweeps) {
  digitalWrite(PIN_CE, LOW);
  memset(out, 0, 126 * sizeof(uint16_t));
  for (uint16_t s = 0; s < sweeps; s++) {
    for (uint8_t ch = 0; ch < 126; ch++) {
      writeReg(REG_RF_CH, ch);
      digitalWrite(PIN_CE, HIGH);
      delayMicroseconds(170);
      digitalWrite(PIN_CE, LOW);
      if (readReg(REG_RPD) & 0x01) out[ch]++;
    }
  }
  applySniffConfig();
}
void printScan() {
  Serial.println(F("channel (freq): activity [background]"));
  bool any = false;
  for (uint8_t ch = 0; ch < 126; ch++) {
    int b = haveBase ? base[ch] : 0;
    int d = (int)hits[ch] - b;
    if (d > 3) {
      any = true;
      Serial.printf("ch %3u (%u MHz): %4u [%u] ", ch, 2400 + ch, hits[ch], b);
      int bars = min(d / 3, 40);
      for (int i = 0; i < bars; i++) Serial.print('#');
      Serial.println();
    }
  }
  if (!any) Serial.println(F("No clear activity. Hold the remote close and keep the button pressed."));
}

// ---------- raw dump + repeat filter ----------
struct Pkt { uint8_t d[32]; uint32_t t; bool shown; };
Pkt hist[32];
uint8_t histPos = 0;
const uint8_t CMP = 10;

void printPkt(const uint8_t *d) {
  Serial.printf("[%8lu] ch%u r%u a%u: ", millis(), channel, rate, addrVariant);
  for (uint8_t i = 0; i < 32; i++) Serial.printf("%02X ", d[i]);
  Serial.println();
}
bool isJunk(const uint8_t *d) {
  bool all0 = true, allF = true;
  for (uint8_t i = 0; i < CMP; i++) {
    if (d[i] != 0x00) all0 = false;
    if (d[i] != 0xFF) allF = false;
  }
  return all0 || allF;
}

void handlePacket(const uint8_t *d) {
  if (xn297Decode) {
    uint32_t h = tryXn297(d);  // works for both bait (slides) and targeted (off=0)
    if (h) {
      uint32_t now = millis();
      if (!(h == lastHash && now - lastTime < 400)) { lastHash = h; lastTime = now; }
    }
    return;
  }
  // Decoder off: raw 32-byte hex, optionally only showing repeated packets
  // (a real frame repeats; one-off lines are usually noise).
  if (!filterOn) { printPkt(d); return; }
  if (isJunk(d)) return;
  uint32_t now = millis();
  for (uint8_t i = 0; i < 32; i++) {
    if (now - hist[i].t < 500 && memcmp(hist[i].d, d, CMP) == 0) {
      if (!hist[i].shown) { printPkt(d); hist[i].shown = true; }
      hist[i].t = now;
      return;
    }
  }
  memcpy(hist[histPos].d, d, 32);
  hist[histPos].t = now;
  hist[histPos].shown = false;
  histPos = (histPos + 1) % 32;
}

// ---------- console ----------
void help() {
  Serial.println(F("\nCommands: b (background scan), s (scan while pressing), c N (channel),"));
  Serial.println(F("r N (0=1M,1=2M,2=250k), a N (bait addr 0..3), f (repeat filter),"));
  Serial.println(F("x (XN297 decode on/off), t (targeted mode), p (sniff start/stop), # label, ? help"));
  Serial.printf("Now: channel %u (%u MHz), rate %u, bait %u, filter %s, decode %s, targeted %s, sniffing %s\n",
                channel, 2400 + channel, rate, addrVariant, filterOn ? "on" : "off",
                xn297Decode ? "on" : "off", targeted ? "on" : "off", sniffing ? "on" : "off");
}

String inputBuf;
void processLine(String line) {
  line.trim();
  if (line.length() == 0) return;
  if (line[0] == '#') { Serial.println(F("------------------------------")); Serial.println(line); return; }
  char c = line[0];
  int arg = line.length() > 1 ? line.substring(1).toInt() : -1;
  switch (c) {
    case 'b': Serial.println(F("Background scan, do NOT touch the remote..."));
              doScan(base, 300); haveBase = true; Serial.println(F("Done.")); break;
    case 's': Serial.println(F("Scan: hold/press the remote button near the module..."));
              doScan(hits, 300); printScan(); break;
    case 'c': if (arg >= 0 && arg <= 125) { channel = arg; applySniffConfig(); } break;
    case 'r': if (arg >= 0 && arg <= 2)   { rate = arg;    applySniffConfig(); } break;
    case 'a': if (arg >= 0 && arg <= 3)   { addrVariant = arg; applySniffConfig(); } break;
    case 'f': filterOn = !filterOn; break;
    case 'x': xn297Decode = !xn297Decode; break;
    case 't': targeted = !targeted; applySniffConfig(); break;
    case 'p': sniffing = !sniffing; applySniffConfig(); break;
  }
  help();
}
void handleCommand() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r' || ch == '\n') {
      if (inputBuf.length() > 0) { String l = inputBuf; inputBuf = ""; processLine(l); }
    } else if (inputBuf.length() < 64) {
      inputBuf += ch;
    }
  }
}

void setup() {
  Serial.begin(115200);
  // Native USB: the port disappears on reset. Wait up to 5 s for the monitor to
  // reconnect, otherwise the startup banner is printed into the void.
  for (uint32_t t0 = millis(); !Serial && millis() - t0 < 5000;) delay(10);
  pinMode(PIN_CE, OUTPUT);
  pinMode(PIN_CSN, OUTPUT);
  digitalWrite(PIN_CE, LOW);
  digitalWrite(PIN_CSN, HIGH);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN);
  delay(200);
  if (!radioInit()) {
    Serial.println(F("nRF24 not responding! Check wiring and power (add the capacitor)."));
    while (true) delay(1000);
  }
  Serial.println(F("nRF24 found. XN297 remote analyzer (receive only)."));
  help();
}

void loop() {
  if (Serial.available()) handleCommand();
  if (sniffing && (readReg(REG_FIFO_STATUS) & 0x01) == 0) {
    uint8_t d[32];
    readPayload(d, 32);
    writeReg(REG_STATUS, 0x40);
    handlePacket(d);
  }
}
