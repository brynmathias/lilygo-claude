// Per-screen panel calibration for our T-Keyboard-S3-Pro (main unit +
// one expansion unit), measured with the spike's calibration mode on
// 2026-11-11. Values saved on the device (NVS) take precedence; these are
// the defaults for a fresh flash or a wiped device.
//
// Keyed by unit address and screen bit, so a different expansion unit
// would need recalibrating.
#pragma once
#include <stdint.h>

struct ScreenCal {
  uint8_t unit; // I2C address of the unit controller
  uint8_t bit;  // screen's bit in REG_LCD_CS
  uint8_t col, row;
  int8_t tilt; // degrees, clockwise positive
};

constexpr ScreenCal SCREEN_CAL[] = {
    // main unit (0x01): keys 1-4
    {0x01, 0b10000, 1, 0, 0},
    {0x01, 0b01000, 2, 1, 0},
    {0x01, 0b00100, 2, 0, 0},
    {0x01, 0b00010, 2, 0, 0},
    // expansion unit (0x02): keys 5-9
    {0x02, 0b10000, 2, 0, 0},
    {0x02, 0b01000, 2, 1, 0},
    {0x02, 0b00100, 2, 0, 0},
    {0x02, 0b00010, 3, 1, 0},
    {0x02, 0b00001, 2, 1, 0},
};

// For a screen not in the table (e.g. another expansion unit).
constexpr uint8_t DEFAULT_COL = 2;
constexpr uint8_t DEFAULT_ROW = 1;
