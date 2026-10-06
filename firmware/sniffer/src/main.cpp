#include <Arduino.h>
/*
  nRF24 sniffer для 2.4 ГГц пульта люстры (ESP32 + nRF24L01+)

  Подключение ниже для классического ESP32; для ESP32-S3 см. пины в #if ниже
  (S3: CE=9, CSN=10, MOSI=11, SCK=12, MISO=13).
  Классический ESP32 (VSPI):
    nRF24 VCC  -> 3V3   (+ конденсатор 10-100 мкФ между VCC и GND у самого модуля!)
    nRF24 GND  -> GND
    nRF24 CE   -> GPIO4
    nRF24 CSN  -> GPIO5
    nRF24 SCK  -> GPIO18
    nRF24 MOSI -> GPIO23
    nRF24 MISO -> GPIO19
    nRF24 IRQ  -> не подключать

  Монитор порта: 115200, окончание строки "Newline" (NL).

  Команды:
    b        - фоновый скан (НЕ нажимать пульт), ~10 с
    s        - скан с нажатиями (зажимайте кнопку пульта рядом с модулем), ~10 с
    c N      - установить канал N (0..125)
    r N      - скорость: 0 = 1 Мбит/с, 1 = 2 Мбит/с, 2 = 250 кбит/с
    a N      - вариант "адреса-приманки" 0..3
    f        - вкл/выкл фильтр (показывать только повторяющиеся пакеты)
    t        - прицельный режим: настоящий адрес пульта вместо приманки (нужна скорость r 2)
    p        - старт/стоп сниффинга
    TX <hex> - передать нажатие кнопки (эмуляция пульта): пачка фазы 00, затем фазы 01.
               Напр. TX 05 = ВКЛ, 09 = ВЫКЛ, 10 = НОЧНИК, 11 = ДЕНЬ. После - снова приём.
    CTR <n>  - задать стартовый счётчик для TX (для проверки replay со старым значением).
               Счётчик хранится в NVS и +1 на каждую пачку (нажатие тратит 2 значения).
    AUTO 0/1 - авто-тест передачи на старте (хранится в NVS, по умолчанию вкл): через 10 с
               после включения шлёт ВКЛ, пауза 8 с, шлёт ВЫКЛ. Для проверки у люстры без ПК.
               Статусный RGB-светодиод: медленно мигает = взведён, горит = идёт передача,
               3 вспышки = пачка отправлена. Жёстко отключить в сборке: -DAUTOTEST_DISABLED
    # текст  - метка в логе (например: # пульт1 вкл/выкл)
    ?        - справка

  Что выяснено про пульт (проверено по CRC на сотнях пакетов):
    Чип XN297L, 250 кбит/с (r 2), скремблирование вкл, преамбула 71 0F 55,
    адрес 5 байт AA 55 CC CC CC, нагрузка 8 байт, CRC16 XN297.
    Каналы (прыгает, пачка ~10 пакетов через 13 мс): 0 2 5 18 21 34 37 45 47 50 53 66 69 82.
    Нагрузка: [фаза] 55 2A 75 00 [команда] [счётчик] [сумма байт 0..6]
      фаза 00 - нажатие, 01 - вторая пачка через ~200 мс (команда | 0x40)
      команды: ВКЛ 05, ВЫКЛ 09, НОЧНИК 10, ДЕНЬ 11; счётчик +1 на каждую пачку
    Быстрый приём: r 2, c 50, t, p
*/

#include <SPI.h>
#include <Preferences.h>

#if defined(CONFIG_IDF_TARGET_ESP32S3)
  // ESP32-S3-DevKitC-1, разъём J1
  #define PIN_CE   9
  #define PIN_CSN  10
  #define PIN_MOSI 11
  #define PIN_SCK  12
  #define PIN_MISO 13
#else
  // Классический ESP32 DevKit
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

#define REG_TX_ADDR     0x10

#define CMD_R_RX_PAYLOAD 0x61
#define CMD_W_TX_PAYLOAD 0xA0
#define CMD_FLUSH_TX     0xE1
#define CMD_FLUSH_RX     0xE2

SPISettings spiCfg(4000000, MSBFIRST, SPI_MODE0);

// "Приманки": 2-байтный адрес, совпадающий с преамбулой 0xAA/0x55 + нулевой шум
const uint8_t ADDR[4][2] = {{0xAA, 0x00}, {0x55, 0x00}, {0x00, 0xAA}, {0x00, 0x55}};

uint8_t channel = 2;
uint8_t rate = 0;
uint8_t addrVariant = 0;
bool filterOn = true;
bool sniffing = false;

// "Прицельный" режим: вместо приманки - настоящий 5-байтный адрес пульта в том
// виде, как он идёт в эфире (XN297-скремблированный CC CC CC 55 AA).
// Пакеты приходят выровненными по байтам, шум не ловится.
bool targeted = false;
const uint8_t TARGET_ADDR[5] = {0x2F, 0xBF, 0x87, 0x7D, 0x2F};  // в регистр - младший байт первым

uint16_t hits[126];
uint16_t base[126];
bool haveBase = false;

// ---------- передача (эмуляция пульта) ----------
// Каналы, по которым прыгает пульт. Пачку шлём по всему списку, чтобы попасть
// на тот канал, который слушает люстра в данный момент.
const uint8_t CHANNELS[] = {0, 2, 5, 18, 21, 34, 37, 45, 47, 50, 53, 66, 69, 82};
const uint8_t NCH = sizeof(CHANNELS);

// Счётчик (один байт в эфире) хранится в NVS и растёт с каждой пачкой.
// Команда CTR его переопределяет - для проверки replay со старым значением.
Preferences prefs;
uint32_t counter = 0;

// Авто-тест на старте: удобно проверить передачу у самой люстры без компьютера.
// Хранится в NVS (по умолчанию вкл); выключается командой AUTO 0 или флагом сборки.
bool autoTest = true;

// ---------- низкоуровневый SPI ----------
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

// ---------- настройка радио ----------
void setRate(uint8_t r) {
  uint8_t v = (r == 1) ? 0x08 : (r == 2) ? 0x20 : 0x00;
  writeReg(REG_RF_SETUP, v | 0x06);
}

void applySniffConfig() {
  digitalWrite(PIN_CE, LOW);
  if (targeted) {
    writeReg(REG_SETUP_AW, 0x03);  // обычный 5-байтный адрес
    writeRegBuf(REG_RX_ADDR_P0, TARGET_ADDR, 5);
  } else {
    writeReg(REG_SETUP_AW, 0x00);  // 2-байтный адрес (недокументированный режим)
    writeRegBuf(REG_RX_ADDR_P0, ADDR[addrVariant], 2);
  }
  setRate(rate);
  writeReg(REG_RF_CH, channel);
  command(CMD_FLUSH_RX);
  writeReg(REG_STATUS, 0x70);
  if (sniffing) digitalWrite(PIN_CE, HIGH);
}

bool radioInit() {
  writeReg(REG_SETUP_AW, 0x03);
  if (readReg(REG_SETUP_AW) != 0x03) return false;

  writeReg(REG_CONFIG, 0x00);
  writeReg(REG_EN_AA, 0x00);       // без автоподтверждения
  writeReg(REG_EN_RXADDR, 0x01);
  writeReg(REG_SETUP_RETR, 0x00);
  writeReg(REG_DYNPD, 0x00);
  writeReg(REG_FEATURE, 0x00);
  writeReg(REG_RX_PW_P0, 32);      // читаем максимум байт
  writeReg(REG_CONFIG, 0x03);      // PWR_UP + PRIM_RX, CRC выключен
  delay(5);
  applySniffConfig();
  return true;
}

// ---------- сканер каналов ----------
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
  Serial.println(F("Канал (частота): активность [фон]"));
  bool any = false;
  for (uint8_t ch = 0; ch < 126; ch++) {
    int b = haveBase ? base[ch] : 0;
    int d = (int)hits[ch] - b;
    if (d > 3) {
      any = true;
      Serial.printf("ch %3u (%u МГц): %4u [%u] ", ch, 2400 + ch, hits[ch], b);
      int bars = min(d / 3, 40);
      for (int i = 0; i < bars; i++) Serial.print('#');
      Serial.println();
    }
  }
  if (!any) Serial.println(F("Заметной активности не найдено. Держите пульт ближе и зажимайте кнопку."));
}

// ---------- фильтр повторов ----------
struct Pkt { uint8_t d[32]; uint32_t t; bool shown; };
Pkt hist[32];
uint8_t histPos = 0;
const uint8_t CMP = 10;

void printPkt(const uint8_t *d) {
  Serial.printf("[%8lu] ch%u r%u a%u: ", millis(), channel, rate, addrVariant);
  for (uint8_t i = 0; i < 32; i++) Serial.printf("%02X ", d[i]);
  Serial.println();
}

// ---------- XN297 декодер ----------
// XN297 перемешивает биты (bit-reverse каждого байта) и XOR-ит со скремблером.
// Мы скользим по всем битовым сдвигам, расшифровываем и проверяем CRC16.
// Совпадение CRC = гарантия, что декод верный и выровненный.
bool xn297Decode = true;

static const uint8_t XN_SCRAMBLE[] = {
  0xe3,0xb1,0x4b,0xea,0x85,0xbc,0xe5,0x66,0x0d,0xae,0x8c,0x88,0x12,0x69,0xee,0x1f,
  0xc7,0x62,0x97,0xd5,0x0b,0x79,0xca,0xcc,0x1b,0x5d,0x19,0x10,0x24,0xd3,0xdc,0x3f,
  0x8e,0xc5,0x2f,0xaa,0x16,0xf3,0x95 };
// Индекс = длина адреса - 3 + длина нагрузки
static const uint16_t XN_XOROUT[] = {
  0x0000,0x3448,0x9BA7,0x8BBB,0x85E1,0x3E8C,0x451E,0x18E6,0x6B24,0xE7AB,0x3828,
  0x814B,0xD461,0xF494,0x2503,0x691D,0xFE8B,0x9BA7,0x8B17,0x2920,0x8B5F,0x61B1,
  0xD391,0x7401,0x2138,0x129F,0xB3A0,0x2988,0x23CA,0xC0CB,0x0C6C,0xB329,0xA0A1,
  0x0A16,0xA9D0 };

static uint8_t br(uint8_t b) {
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

// Пытается декодировать XN297 из сырого буфера. Печатает все валидные кадры.
// Возвращает хэш первого валидного кадра (для фильтра повторов) или 0.
uint32_t tryXn297(const uint8_t *raw) {
  // Разворачиваем 32 байта в биты
  uint8_t bits[256];
  for (int i = 0; i < 256; i++) bits[i] = (raw[i >> 3] >> (7 - (i & 7))) & 1;

  for (int off = 0; off <= 256 - 56; off++) {
    // Собираем до 30 байт начиная с этого битового сдвига
    uint8_t seg[30];
    int avail = (256 - off) / 8;
    if (avail > 30) avail = 30;
    for (int k = 0; k < avail; k++) {
      uint8_t v = 0;
      for (int b = 0; b < 8; b++) v = (v << 1) | bits[off + k * 8 + b];
      seg[k] = v;
    }
    // Перебираем длину адреса и полезной нагрузки
    for (int alen = 3; alen <= 5; alen++) {
      for (int plen = 1; plen <= 16; plen++) {
        int total = alen + plen;
        if (total + 2 > avail) continue;
        // CRC считается по байтам как в эфире (скремблированным) и сам не скремблируется.
        // Адрес: без bit-reverse, в обратном порядке. Нагрузка: XOR, затем bit-reverse.
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

// Адрес пульта, как он идёт в эфире (TARGET_ADDR в обратном порядке), и длина нагрузки
static const uint8_t TARGET_AIR[5] = {0x2F, 0x7D, 0x87, 0xBF, 0x2F};
static const uint8_t TARGET_PLEN = 8;

bool isJunk(const uint8_t *d) {
  bool all0 = true, allF = true;
  for (uint8_t i = 0; i < CMP; i++) {
    if (d[i] != 0x00) all0 = false;
    if (d[i] != 0xFF) allF = false;
  }
  return all0 || allF;
}

uint32_t lastXnHash = 0;
uint32_t lastXnTime = 0;

void handlePacket(const uint8_t *d) {
  if (targeted) {
    // Сырые байты после адреса (для tools/tframes.py)...
    Serial.printf("[%8lu] ch%u r%u T: ", millis(), channel, rate);
    for (uint8_t i = 0; i < 16; i++) Serial.printf("%02X ", d[i]);
    Serial.println();
    // ...и расшифровка: 8 байт нагрузки + CRC16
    uint16_t crc = xnCrc(d, TARGET_PLEN, xnCrc(TARGET_AIR, 5)) ^ XN_XOROUT[5 - 3 + TARGET_PLEN];
    bool ok = crc == ((uint16_t)(d[TARGET_PLEN] << 8) | d[TARGET_PLEN + 1]);
    Serial.printf("[%8lu] XN297 %s addr AA55CCCCCC | payload ", millis(), ok ? "OK" : "CRC ERR");
    for (uint8_t i = 0; i < TARGET_PLEN; i++) Serial.printf("%02X ", br(d[i] ^ XN_SCRAMBLE[5 + i]));
    Serial.println();
    return;
  }
  if (xn297Decode) {
    uint32_t h = tryXn297(d);
    if (h) {
      // показываем, но подавляем дубликаты в пределах 400 мс
      uint32_t now = millis();
      if (!(h == lastXnHash && now - lastXnTime < 400)) {
        lastXnHash = h; lastXnTime = now;
      }
    }
    return;  // в режиме XN297 сырые пакеты не печатаем
  }
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

// ---------- индикатор (встроенный RGB-светодиод) ----------
void ledSet(bool on) {
#ifdef RGB_BUILTIN
  rgbLedWrite(RGB_BUILTIN, on ? 12 : 0, on ? 12 : 0, on ? 28 : 0);  // тускло-синий
#elif defined(LED_BUILTIN)
  digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
#else
  (void)on;
#endif
}
void ledBlink(uint8_t n, uint16_t on_ms = 120, uint16_t off_ms = 120) {
  for (uint8_t i = 0; i < n; i++) { ledSet(true); delay(on_ms); ledSet(false); delay(off_ms); }
}

// ---------- передатчик ----------
void writeTxPayload(const uint8_t *b, uint8_t n) {
  SPI.beginTransaction(spiCfg);
  digitalWrite(PIN_CSN, LOW);
  SPI.transfer(CMD_W_TX_PAYLOAD);
  for (uint8_t i = 0; i < n; i++) SPI.transfer(b[i]);
  digitalWrite(PIN_CSN, HIGH);
  SPI.endTransaction();
}

// Переключаем радио в режим передачи XN297. Приём на это время останавливается.
// Трюк XN297: преамбула 71 0F 55 кладётся в аппаратный TX_ADDR, а настоящий
// скремблированный адрес пульта идёт первыми байтами полезной нагрузки FIFO.
void radioTxSetup() {
  digitalWrite(PIN_CE, LOW);
  writeReg(REG_CONFIG, 0x02);      // PWR_UP + PRIM_TX, аппаратный CRC выключен
  delay(2);
  setRate(2);                      // 250 кбит/с, как у пульта
  writeReg(REG_EN_AA, 0x00);
  writeReg(REG_SETUP_RETR, 0x00);
  writeReg(REG_SETUP_AW, 0x03);    // 5-байтный "адрес" = преамбула XN297
  const uint8_t pre[5] = {0x55, 0x0F, 0x71, 0x0C, 0x00};
  writeRegBuf(REG_TX_ADDR, pre, 5);
  command(CMD_FLUSH_TX);
  writeReg(REG_STATUS, 0x70);
}

// Один кадр: адрес в эфире + 8 байт нагрузки + CRC16, собранный ровно обратно
// приёмному декодеру (br + скремблер + тот же xnCrc/XN_XOROUT).
void txFrame(uint8_t ch, const uint8_t payload8[8]) {
  uint8_t buf[15];
  for (uint8_t i = 0; i < 5; i++) buf[i] = TARGET_AIR[i];
  for (uint8_t i = 0; i < 8; i++) buf[5 + i] = br(payload8[i]) ^ XN_SCRAMBLE[5 + i];
  uint16_t crc = xnCrc(buf, 13) ^ XN_XOROUT[5 - 3 + 8];  // над адресом+нагрузкой
  buf[13] = crc >> 8;
  buf[14] = crc & 0xFF;

  digitalWrite(PIN_CE, LOW);
  writeReg(REG_RF_CH, ch);
  command(CMD_FLUSH_TX);
  writeReg(REG_STATUS, 0x70);
  writeTxPayload(buf, 15);
  digitalWrite(PIN_CE, HIGH);
  delayMicroseconds(20);           // > 10 мкс импульс CE запускает передачу
  digitalWrite(PIN_CE, LOW);
  uint32_t t0 = micros();          // ждём TX_DS (кадр ушёл)
  while (!(readReg(REG_STATUS) & 0x20) && micros() - t0 < 3000) {}
  writeReg(REG_STATUS, 0x70);
}

// Пачка из ~одного прохода по всем каналам, счётчик в пачке постоянный.
void txBurst(uint8_t phase, uint8_t cmd, uint8_t ctr) {
  uint8_t pl[8] = {phase, 0x55, 0x2A, 0x75, 0x00, cmd, ctr, 0};
  uint16_t s = 0;
  for (uint8_t i = 0; i < 7; i++) s += pl[i];
  pl[7] = s & 0xFF;                // контрольная сумма = сумма байт 0..6

  Serial.printf("  TX пачка фаза %02X: ", phase);
  for (uint8_t i = 0; i < 8; i++) Serial.printf("%02X ", pl[i]);
  Serial.printf(" -> каналы");
  ledSet(true);                    // светодиод горит, пока идёт пачка = видно передачу
  for (uint8_t i = 0; i < NCH; i++) {
    Serial.printf(" %u", CHANNELS[i]);
    txFrame(CHANNELS[i], pl);
    delayMicroseconds(800);
  }
  ledSet(false);
  Serial.println();
}

// Полное "нажатие": пачка фазы 00, затем через ~200 мс пачка фазы 01 (команда|0x40).
void doTx(uint8_t cmd) {
  bool wasSniff = sniffing;
  Serial.printf("[%8lu] Передаю команду %02X, стартовый счётчик %u (0x%02X)\n",
                millis(), cmd, (unsigned)counter, counter & 0xFF);
  radioTxSetup();

  txBurst(0x00, cmd, counter & 0xFF);
  counter++;
  delay(200);
  txBurst(0x01, cmd | 0x40, counter & 0xFF);
  counter++;

  prefs.putUInt("ctr", counter);

  writeReg(REG_CONFIG, 0x03);      // назад в PWR_UP + PRIM_RX
  delay(2);
  sniffing = wasSniff;
  applySniffConfig();              // восстановит канал/адрес и приём
  Serial.printf("[%8lu] TX готово. Счётчик теперь %u (0x%02X). Приём возобновлён.\n",
                millis(), (unsigned)counter, counter & 0xFF);
}

// Авто-тест на старте: взвестись, передать ВКЛ, пауза, передать ВЫКЛ. Для проверки
// передачи у люстры без компьютера. Каждый кадр печатается в Serial (если подключён).
void runAutoTest() {
  // После теста хотим штатный приём на подтверждённых настройках.
  rate = 2; channel = 50; targeted = true; sniffing = true;

  Serial.println(F("=== АВТО-ТЕСТ ПЕРЕДАЧИ. Выключить потом: AUTO 0 ==="));
  Serial.println(F("Взведён. 10 с до ВКЛ (медленное мигание)..."));
  for (int i = 0; i < 20; i++) { ledSet(i & 1); delay(500); }
  ledSet(false);

  Serial.println(F("--- Передаю ВКЛ (TX 05) ---"));
  doTx(0x05);
  ledBlink(3);

  Serial.println(F("Пауза 8 с до ВЫКЛ..."));
  for (int i = 0; i < 16; i++) { ledSet(i & 1); delay(500); }
  ledSet(false);

  Serial.println(F("--- Передаю ВЫКЛ (TX 09) ---"));
  doTx(0x09);
  ledBlink(3);

  Serial.println(F("Авто-тест завершён. Обычный приём (r2 c50 прицельный)."));
}

// ---------- команды ----------
void help() {
  Serial.println(F("\nКоманды: b (фон), s (скан с нажатиями), c N (канал), r N (0=1M,1=2M,2=250k),"));
  Serial.println(F("a N (адрес 0..3), f (фильтр), x (XN297 декод вкл/выкл), t (прицельный режим), p (сниффинг старт/стоп), # метка, ? справка"));
  Serial.println(F("TX <hex> - передать нажатие (05=ВКЛ 09=ВЫКЛ 10=НОЧНИК 11=ДЕНЬ), CTR <n> - задать счётчик"));
  Serial.printf("AUTO 0/1 - авто-тест передачи на старте (%s)\n", autoTest ? "вкл" : "выкл");
  Serial.printf("Сейчас: канал %u, скорость %u, адрес %u, фильтр %s, XN297 %s, прицельный %s, сниффинг %s\n",
                channel, rate, addrVariant, filterOn ? "вкл" : "выкл",
                xn297Decode ? "вкл" : "выкл", targeted ? "вкл" : "выкл", sniffing ? "вкл" : "выкл");
}

String inputBuf;
void processLine(String line);

// Собираем символы до Enter (терминал может слать их по одному)
void handleCommand() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r' || ch == '\n') {
      if (inputBuf.length() > 0) {
        String l = inputBuf;
        inputBuf = "";
        processLine(l);
      }
    } else if (inputBuf.length() < 64) {
      inputBuf += ch;
    }
  }
}

void processLine(String line) {
  line.trim();
  if (line.length() == 0) return;
  if (line[0] == '#') { Serial.println(F("------------------------------")); Serial.println(line); return; }

  // Многобуквенные команды передачи - до разбора однобуквенных.
  String up = line;
  up.toUpperCase();
  if (up.startsWith("TX")) {
    String a = line.substring(2);
    a.trim();
    if (a.length() == 0) { Serial.println(F("Использование: TX <команда hex>, напр. TX 05 (ВКЛ)")); return; }
    uint8_t cmd = (uint8_t)strtol(a.c_str(), nullptr, 16);
    doTx(cmd);
    return;
  }
  if (up.startsWith("CTR")) {
    String a = line.substring(3);
    a.trim();
    if (a.length() == 0) { Serial.printf("Счётчик сейчас %u (0x%02X)\n", (unsigned)counter, counter & 0xFF); return; }
    counter = (uint32_t)strtoul(a.c_str(), nullptr, 10);
    prefs.putUInt("ctr", counter);
    Serial.printf("Счётчик установлен в %u (0x%02X)\n", (unsigned)counter, counter & 0xFF);
    return;
  }
  if (up.startsWith("AUTO")) {
    String a = line.substring(4);
    a.trim();
    if (a.length() == 0) { Serial.printf("Авто-тест на старте: %s\n", autoTest ? "вкл" : "выкл"); return; }
    autoTest = a.toInt() != 0;
    prefs.putUChar("autotest", autoTest ? 1 : 0);
    Serial.printf("Авто-тест на старте: %s\n", autoTest ? "вкл" : "выкл");
    return;
  }

  char c = line[0];
  int arg = line.length() > 1 ? line.substring(1).toInt() : -1;

  switch (c) {
    case 'b':
      Serial.println(F("Фоновый скан, НЕ трогайте пульт..."));
      doScan(base, 300); haveBase = true;
      Serial.println(F("Готово."));
      break;
    case 's':
      Serial.println(F("Скан: зажимайте/нажимайте кнопку пульта рядом с модулем..."));
      doScan(hits, 300); printScan();
      break;
    case 'c':
      if (arg >= 0 && arg <= 125) { channel = arg; applySniffConfig(); }
      break;
    case 'r':
      if (arg >= 0 && arg <= 2) { rate = arg; applySniffConfig(); }
      break;
    case 'a':
      if (arg >= 0 && arg <= 3) { addrVariant = arg; applySniffConfig(); }
      break;
    case 'f':
      filterOn = !filterOn;
      break;
    case 'p':
      sniffing = !sniffing; applySniffConfig();
      break;
    case 'x':
      xn297Decode = !xn297Decode;
      break;
    case 't':
      targeted = !targeted; applySniffConfig();
      break;
  }
  help();
}

void setup() {
  Serial.begin(115200);
  // Встроенный USB: порт пропадает при сбросе. Ждём до 5 с, пока монитор
  // переподключится, иначе стартовое сообщение уйдёт в никуда.
  for (uint32_t t0 = millis(); !Serial && millis() - t0 < 5000;) delay(10);
  pinMode(PIN_CE, OUTPUT);
  pinMode(PIN_CSN, OUTPUT);
  digitalWrite(PIN_CE, LOW);
  digitalWrite(PIN_CSN, HIGH);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN);
  delay(200);

  prefs.begin("chand", false);
  counter = prefs.getUInt("ctr", 0);
  autoTest = prefs.getUChar("autotest", 1) != 0;
  ledSet(false);

  if (!radioInit()) {
    Serial.println(F("nRF24 не отвечает! Проверьте проводку и питание (конденсатор)."));
    while (true) delay(1000);
  }
  Serial.println(F("nRF24 найден."));
  help();

#ifndef AUTOTEST_DISABLED
  if (autoTest) runAutoTest();
#endif
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
