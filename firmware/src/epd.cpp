#include "epd.h"

namespace {

constexpr int PIN_PANEL_POWER = 7;
constexpr int PIN_POWER = 41;
constexpr int PIN_SCK = 12;
constexpr int PIN_MOSI = 11;
constexpr int PIN_CS = 45;
constexpr int PIN_DC = 46;
constexpr int PIN_RST = 47;
constexpr int PIN_BUSY = 48;

// Full refresh waveform (registers 0x20-0x24, 42 bytes each, only the first 7 are non-zero)
constexpr uint8_t LUT_GC[5][7] = {
    {0x01, 0x14, 0x0A, 0x14, 0x00, 0x01, 0x01},
    {0x01, 0x54, 0x0A, 0x94, 0x00, 0x01, 0x01},
    {0x01, 0x54, 0x0A, 0x94, 0x00, 0x01, 0x01},
    {0x01, 0x94, 0x0A, 0x54, 0x00, 0x01, 0x01},
    {0x01, 0x94, 0x0A, 0x54, 0x00, 0x01, 0x01},
};
constexpr int LUT_SIZE = 42;

// Bit-banged SPI, CS low for each byte
void writeByte(uint8_t b) {
  digitalWrite(PIN_CS, LOW);
  for (int i = 0; i < 8; i++) {
    digitalWrite(PIN_SCK, LOW);
    digitalWrite(PIN_MOSI, (b & 0x80) ? HIGH : LOW);
    digitalWrite(PIN_SCK, HIGH);
    b <<= 1;
  }
  digitalWrite(PIN_CS, HIGH);
}

void command(uint8_t cmd, std::initializer_list<uint8_t> data = {}) {
  digitalWrite(PIN_DC, LOW);
  writeByte(cmd);
  digitalWrite(PIN_DC, HIGH);
  for (uint8_t d : data) writeByte(d);
}

// BUSY is low while the panel works (Elecrow's demo waits for low, so it never waits)
bool waitIdle(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (digitalRead(PIN_BUSY) == LOW) {
    if (millis() - start > timeoutMs) return false;
    delay(5);
  }
  Serial.printf("epd: busy for %lu ms\n", (unsigned long)(millis() - start));
  return true;
}

void reset() {
  digitalWrite(PIN_RST, HIGH);
  delay(10);
  digitalWrite(PIN_RST, LOW);
  delay(100);
  digitalWrite(PIN_RST, HIGH);
  delay(100);
}

void configure() {
  command(0x00, {0x3F, 0x4D});                    // panel setting
  command(0x01, {0x03, 0x10, 0x3F, 0x3F, 0x03});  // power setting
  command(0x06, {0x96, 0x96, 0x29});              // booster soft start
  command(0x30, {0x09});                          // PLL
  command(0x61, {0x01, 0x90, 0x01, 0x2C});        // resolution 400 x 300
  command(0x82, {0x05});                          // VCOM DC
  command(0x50, {0x97});                          // VCOM and data interval
  command(0x60, {0x22});                          // TCON
  command(0xE3, {0x88});                          // power saving
}

void loadLut() {
  for (int i = 0; i < 5; i++) {
    command(0x20 + i);
    for (int j = 0; j < LUT_SIZE; j++) writeByte(j < 7 ? LUT_GC[i][j] : 0x00);
  }
}

}  // namespace

namespace epd {

void begin() {
  pinMode(PIN_POWER, OUTPUT);
  digitalWrite(PIN_POWER, HIGH);
  pinMode(PIN_PANEL_POWER, OUTPUT);
  digitalWrite(PIN_PANEL_POWER, HIGH);

  pinMode(PIN_SCK, OUTPUT);
  pinMode(PIN_MOSI, OUTPUT);
  pinMode(PIN_CS, OUTPUT);
  pinMode(PIN_DC, OUTPUT);
  pinMode(PIN_RST, OUTPUT);
  pinMode(PIN_BUSY, INPUT);
  digitalWrite(PIN_CS, HIGH);
}

bool show(const uint8_t *bitmap) {
  reset();
  configure();
  delay(300);

  command(0x50, {0xD7});  // VCOM and data interval for the refresh
  command(0x13);          // new image data
  for (int i = 0; i < WIDTH * HEIGHT / 8; i++) writeByte(bitmap[i]);
  loadLut();

  command(0x17, {0xA5});  // auto sequence: power on, refresh, power off
  delay(10);  // BUSY goes low shortly after the command
  bool ok = waitIdle(20000);
  command(0x07, {0xA5});  // deep sleep, the next show() resets the panel
  delay(50);
  return ok;
}

}  // namespace epd
