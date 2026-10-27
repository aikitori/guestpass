// Panel driver for the CrowPanel ESP32 4.2" E-Paper, PCB V1.2A (green sticker on the back).
// The V1.2A panel uses a UC81xx-style command set, so SSD1683 drivers (GxEPD2) do not fit.
// The command sequence follows Elecrow's demo for this board revision.
#pragma once
#include <Arduino.h>

namespace epd {

constexpr int WIDTH = 400;
constexpr int HEIGHT = 300;

// Switches on the panel power and sets up the pins
void begin();

// Full refresh with a WIDTH x HEIGHT bitmap: MSB first, 1 = white (the GFXcanvas1 layout).
// Puts the panel to sleep afterwards. Returns false if the panel stayed busy.
bool show(const uint8_t *bitmap);

}  // namespace epd
