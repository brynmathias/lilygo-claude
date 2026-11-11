// Spike: prove the T-Keyboard-S3-Pro hardware paths before the real firmware,
// and calibrate each screen (offset and tilt).
// Design: docs/architecture/keypads.md (Delivery order, step 1; AC 1).
//
// Reported on USB serial (115200):
//   UNITS ...   unit controllers found (main = 0x01, expansion = others),
//               at boot and whenever the set changes (checked every second)
//   RAW ...     a unit's key bitmask changed (it is level: held keys stay set)
//   KNOB ...    knob turns (+n/-n) and press (P D / P U, GPIO0)
//   CAL ...     a screen's calibration changed (also saved to flash)
//
// Calibration (always on in the spike):
//   pick a screen: press its key, or n / p (or Tab) over serial
//   knob turn: adjusts the current axis; knob press: next axis, cycling
//              left/right -> up/down -> tilt (back to left/right on a new key)
//   serial: arrow keys or w a s d = offset, [ ] = tilt
//   P prints every screen's calibration; r redraws; R rescans units

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <Wire.h>
#include <math.h>

#include "calibration.h"

// Pins: LilyGO libraries/common/pin_config.h @ cb41b37.
constexpr int PIN_I2C_SDA = 42;
constexpr int PIN_I2C_SCL = 2;
constexpr int PIN_LCD_MOSI = 40;
constexpr int PIN_LCD_SCLK = 41;
constexpr int PIN_LCD_DC = 39;
constexpr int PIN_LCD_RST = 38;
constexpr int PIN_LCD_BL = 1; // active-low on our boards
constexpr int PIN_KNOB_A = 4;
constexpr int PIN_KNOB_B = 5;
constexpr int PIN_KNOB_KEY = 0; // also BOOT

// Unit controller (STM32) registers: T-Keyboard-S3-Pro_Drive.h @ cb41b37.
constexpr uint8_t REG_LCD_CS = 0x01;      // write: bitmask of selected screens
constexpr uint8_t REG_KEY_TRIGGER = 0x02; // read: bitmask of keys held
constexpr uint8_t MAIN_UNIT = 0x01;
constexpr uint8_t MAIN_KNOB_BIT = 0b00001; // main unit reports the knob press here

constexpr uint8_t SLOT_BITS[5] = {0b10000, 0b01000, 0b00100, 0b00010, 0b00001};
constexpr uint32_t CS_SETTLE_MS = 20; // vendor's IIC_LCD_CS_DEVICE_DELAY
constexpr uint32_t SPI_HZ = 4500000;  // vendor: max with chained units
constexpr int16_t W = 128, H = 128;
constexpr size_t MAX_UNITS = 6;
constexpr size_t MAX_SCREENS = MAX_UNITS * 5;
constexpr int MAX_OFFSET = 40;
constexpr int MAX_TILT = 15; // degrees either way

// Arduino_ST7735 with offsets that can change per screen. The offsets are
// protected members of Arduino_TFT; rotation 0 reads COL/ROW_OFFSET1.
class TunableST7735 : public Arduino_ST7735 {
public:
  using Arduino_ST7735::Arduino_ST7735;
  void setOffsets(uint8_t col, uint8_t row) {
    COL_OFFSET1 = COL_OFFSET2 = _xStart = col;
    ROW_OFFSET1 = ROW_OFFSET2 = _yStart = row;
    _currentX = _currentY = _currentW = _currentH = 0xFFFF; // force a new window
  }
};

Arduino_DataBus *bus = new Arduino_HWSPI(PIN_LCD_DC, -1, PIN_LCD_SCLK, PIN_LCD_MOSI, -1);
TunableST7735 *gfx =
    new TunableST7735(bus, -1, 0, true, W, H, DEFAULT_COL, DEFAULT_ROW, DEFAULT_COL, DEFAULT_ROW);
// Each screen is drawn here first, then rotated by its tilt into `tilted`.
Arduino_Canvas *canvas = new Arduino_Canvas(W, H, nullptr);
uint16_t *tilted = nullptr;
Preferences prefs;

struct Screen {
  uint8_t unit; // I2C address of its unit controller
  uint8_t bit;  // its bit in REG_LCD_CS and REG_KEY_TRIGGER
  uint8_t col, row;
  int8_t tilt; // degrees, clockwise positive
};

enum Axis { AXIS_COL, AXIS_ROW, AXIS_TILT };
const char *AXIS_NAMES[] = {"left/right", "up/down", "tilt"};

uint8_t units[MAX_UNITS];
size_t unitCount = 0;
uint8_t lastRaw[MAX_UNITS] = {0};
Screen screens[MAX_SCREENS];
size_t screenCount = 0;
size_t selected = 0;
Axis knobAxis = AXIS_COL;

// The unit controller only acts on a write once the next I2C transaction
// arrives (found in the spike; LilyGO's code hides this with a background
// scan loop). So every write is followed by an empty probe.
bool writeReg(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  uint8_t err = Wire.endTransmission();
  if (err) Serial.printf("ERR write addr=0x%02X reg=0x%02X err=%u\n", addr, reg, err);
  Wire.beginTransmission(addr);
  Wire.endTransmission();
  return err == 0;
}

bool readReg(uint8_t addr, uint8_t reg, uint32_t waitMs, uint8_t *out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return false;
  if (waitMs) delay(waitMs);
  if (Wire.requestFrom(addr, (uint8_t)1) != 1) return false;
  *out = Wire.read();
  return true;
}

String prefKey(const Screen &sc) {
  char k[8];
  snprintf(k, sizeof k, "c%02x%02x", sc.unit, sc.bit);
  return String(k);
}

// Stored as one uint32: col << 16 | row << 8 | (tilt + 128).
void loadCal(Screen &sc) {
  uint8_t col = DEFAULT_COL, row = DEFAULT_ROW;
  int8_t tilt = 0;
  for (const ScreenCal &c : SCREEN_CAL)
    if (c.unit == sc.unit && c.bit == sc.bit) {
      col = c.col;
      row = c.row;
      tilt = c.tilt;
    }
  uint32_t def = (col << 16) | (row << 8) | (uint8_t)(tilt + 128);
  uint32_t v = prefs.getULong(prefKey(sc).c_str(), def);
  sc.col = (v >> 16) & 0xFF;
  sc.row = (v >> 8) & 0xFF;
  sc.tilt = (int)(v & 0xFF) - 128;
}

void saveCal(const Screen &sc) {
  prefs.putULong(prefKey(sc).c_str(), (sc.col << 16) | (sc.row << 8) | (uint8_t)(sc.tilt + 128));
}

void printCal(size_t i) {
  const Screen &sc = screens[i];
  Serial.printf("CAL screen=%u unit=0x%02X bit=%u col=%u row=%u tilt=%d\n", (unsigned)i + 1,
                sc.unit, sc.bit, sc.col, sc.row, sc.tilt);
}

// Screens in key order: main unit's 4 (its 5th slot is the knob), then each
// expansion unit's 5.
void buildScreens() {
  screenCount = 0;
  for (size_t u = 0; u < unitCount; u++) {
    int slots = units[u] == MAIN_UNIT ? 4 : 5;
    for (int s = 0; s < slots && screenCount < MAX_SCREENS; s++) {
      Screen &sc = screens[screenCount++];
      sc.unit = units[u];
      sc.bit = SLOT_BITS[s];
      loadCal(sc);
    }
  }
  if (selected >= screenCount) selected = 0;
}

// Returns true if the set of units changed. Main unit (0x01) sorts first.
bool scanUnits() {
  uint8_t found[MAX_UNITS];
  size_t n = 0;
  for (uint8_t a = 1; a < 127 && n < MAX_UNITS; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) found[n++] = a;
  }
  bool changed = n != unitCount || memcmp(found, units, n) != 0;
  if (changed) {
    memcpy(units, found, n);
    unitCount = n;
    memset(lastRaw, 0, sizeof lastRaw);
    Serial.printf("UNITS n=%u layout=%s :", (unsigned)n, n >= 2 ? "9" : "4");
    for (size_t i = 0; i < n; i++) Serial.printf(" 0x%02X", units[i]);
    Serial.println();
    buildScreens();
  }
  return changed;
}

void selectUnits(uint8_t mask) {
  for (size_t i = 0; i < unitCount; i++) {
    writeReg(units[i], REG_LCD_CS, mask);
    delay(10);
  }
}

void initScreens() {
  // Vendor: every panel must be selected while gfx->begin() runs, and again
  // whenever a unit is added.
  selectUnits(0b11111);
  gfx->setOffsets(DEFAULT_COL, DEFAULT_ROW);
  gfx->begin(SPI_HZ);
  gfx->fillScreen(BLACK);
  delay(100);
  selectUnits(0);
}

// Nearest-neighbour rotation of the canvas about its centre. Pixels that
// come from outside the canvas take the background colour.
void rotateInto(const uint16_t *src, uint16_t *dst, int tiltDeg, uint16_t bg) {
  if (tiltDeg == 0) {
    memcpy(dst, src, W * H * 2);
    return;
  }
  float a = -tiltDeg * (float)M_PI / 180.0f; // sample the source backwards
  int32_t c = (int32_t)(cosf(a) * 65536), s = (int32_t)(sinf(a) * 65536);
  const int32_t cx = W / 2, cy = H / 2;
  for (int y = 0; y < H; y++) {
    int32_t dy = y - cy;
    for (int x = 0; x < W; x++) {
      int32_t dx = x - cx;
      int32_t sx = ((dx * c - dy * s) >> 16) + cx;
      int32_t sy = ((dx * s + dy * c) >> 16) + cy;
      dst[y * W + x] = (sx >= 0 && sx < W && sy >= 0 && sy < H) ? src[sy * W + sx] : bg;
    }
  }
}

void drawScreen(size_t i) {
  const Screen &sc = screens[i];
  bool sel = i == selected;
  uint16_t bg = sc.unit == MAIN_UNIT ? NAVY : DARKGREEN;
  uint32_t t0 = millis();

  canvas->fillScreen(bg);
  canvas->drawRect(0, 0, W, H, sel ? YELLOW : WHITE); // edge check
  if (sel) canvas->drawRect(1, 1, W - 2, H - 2, YELLOW);
  canvas->drawFastHLine(10, H / 2, W - 20, DARKGREY); // straightness guide
  canvas->setTextColor(WHITE);
  canvas->setTextSize(6);
  canvas->setCursor(48, 14);
  canvas->print((unsigned)i + 1);
  canvas->setTextSize(1);
  canvas->setCursor(16, 80);
  canvas->printf("c%u r%u t%+d", sc.col, sc.row, sc.tilt);
  if (sel) {
    canvas->setCursor(16, 96);
    canvas->setTextColor(YELLOW);
    canvas->printf("knob: %s", AXIS_NAMES[knobAxis]);
  }
  rotateInto(canvas->getFramebuffer(), tilted, sc.tilt, bg);

  writeReg(sc.unit, REG_LCD_CS, sc.bit);
  delay(CS_SETTLE_MS);
  gfx->setOffsets(sc.col, sc.row);
  gfx->draw16bitRGBBitmap(0, 0, tilted, W, H);
  writeReg(sc.unit, REG_LCD_CS, 0);
  Serial.printf("DRAW n=%u ms=%lu\n", (unsigned)i + 1, (unsigned long)(millis() - t0));
}

void drawAll() {
  for (size_t i = 0; i < screenCount; i++) drawScreen(i);
}

void select(size_t i) {
  if (i >= screenCount || i == selected) return;
  size_t old = selected;
  selected = i;
  knobAxis = AXIS_COL; // each new screen starts on left/right
  drawScreen(old);
  drawScreen(selected);
  printCal(selected);
}

void adjust(int dCol, int dRow, int dTilt) {
  if (!screenCount) return;
  Screen &sc = screens[selected];
  sc.col = constrain((int)sc.col + dCol, 0, MAX_OFFSET);
  sc.row = constrain((int)sc.row + dRow, 0, MAX_OFFSET);
  sc.tilt = constrain((int)sc.tilt + dTilt, -MAX_TILT, MAX_TILT);
  saveCal(sc);
  drawScreen(selected);
  printCal(selected);
}

void pollKeys() {
  for (size_t u = 0; u < unitCount; u++) {
    uint8_t v;
    if (!readReg(units[u], REG_KEY_TRIGGER, 20, &v)) {
      Serial.printf("ERR key read addr=0x%02X\n", units[u]);
      continue;
    }
    if (v == lastRaw[u]) continue;
    Serial.printf("RAW t=%lu addr=0x%02X val=0b", (unsigned long)millis(), units[u]);
    for (int b = 4; b >= 0; b--) Serial.print((v >> b) & 1);
    Serial.println();
    uint8_t pressed = v & ~lastRaw[u];
    lastRaw[u] = v;
    if (units[u] == MAIN_UNIT) pressed &= ~MAIN_KNOB_BIT;
    for (size_t i = 0; i < screenCount; i++)
      if (screens[i].unit == units[u] && (pressed & screens[i].bit)) select(i);
  }
}

// Quadrature decode as in the vendor's KNOB_Logical_Scan_Loop, in an
// interrupt so turns aren't missed while a key read waits on I2C.
volatile int32_t knobSteps = 0;
volatile uint8_t knobPrev = 0;
bool knobKeyPrev = false;

void IRAM_ATTR knobIsr() {
  uint8_t now = (digitalRead(PIN_KNOB_A) << 1) | digitalRead(PIN_KNOB_B);
  if (now == knobPrev) return;
  if (now == 0b10) knobSteps += knobPrev == 0b00 ? 1 : -1;
  else if (now == 0b01) knobSteps += knobPrev == 0b00 ? -1 : 1;
  knobPrev = now;
}

void pollKnob() {
  noInterrupts();
  int32_t steps = knobSteps;
  knobSteps = 0;
  interrupts();
  if (steps) {
    Serial.printf("KNOB %+ld\n", (long)steps);
    if (knobAxis == AXIS_ROW) adjust(0, steps, 0);
    else if (knobAxis == AXIS_COL) adjust(steps, 0, 0);
    else adjust(0, 0, steps);
  }
  bool key = digitalRead(PIN_KNOB_KEY) == LOW;
  if (key != knobKeyPrev) {
    Serial.println(key ? "KNOB P D" : "KNOB P U");
    knobKeyPrev = key;
    if (key && screenCount) {
      knobAxis = (Axis)((knobAxis + 1) % 3);
      drawScreen(selected);
    }
  }
}

// Serial input: single keys, plus arrow keys (ESC [ A/B/C/D).
int escState = 0;

void handleSerial() {
  while (Serial.available()) {
    int c = Serial.read();
    if (escState == 1) {
      escState = c == '[' ? 2 : 0;
      continue;
    }
    if (escState == 2) {
      escState = 0;
      if (c == 'A') adjust(0, -1, 0);
      else if (c == 'B') adjust(0, 1, 0);
      else if (c == 'C') adjust(1, 0, 0);
      else if (c == 'D') adjust(-1, 0, 0);
      continue;
    }
    if (!screenCount && c != 'R') continue;
    switch (c) {
    case 27: escState = 1; break;
    case 'w': adjust(0, -1, 0); break;
    case 's': adjust(0, 1, 0); break;
    case 'd': adjust(1, 0, 0); break;
    case 'a': adjust(-1, 0, 0); break;
    case '[': adjust(0, 0, -1); break;
    case ']': adjust(0, 0, 1); break;
    case 'n': case '\t': select((selected + 1) % screenCount); break;
    case 'p': select((selected + screenCount - 1) % screenCount); break;
    case 'P': for (size_t i = 0; i < screenCount; i++) printCal(i); break;
    case 'r': drawAll(); break;
    case 'R': unitCount = 0; scanUnits(); initScreens(); drawAll(); break;
    default: break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500); // let the host open the port
  Serial.println("SPIKE keypads");

  pinMode(PIN_KNOB_A, INPUT_PULLUP);
  pinMode(PIN_KNOB_B, INPUT_PULLUP);
  pinMode(PIN_KNOB_KEY, INPUT_PULLUP);
  knobPrev = (digitalRead(PIN_KNOB_A) << 1) | digitalRead(PIN_KNOB_B);
  attachInterrupt(PIN_KNOB_A, knobIsr, CHANGE);
  attachInterrupt(PIN_KNOB_B, knobIsr, CHANGE);

  pinMode(PIN_LCD_RST, OUTPUT);
  digitalWrite(PIN_LCD_RST, HIGH);
  uint32_t rstReleased = millis();
  ledcSetup(1, 20000, 8);
  ledcAttachPin(PIN_LCD_BL, 1);
  ledcWrite(1, 0); // 0 = full brightness (active-low)

  canvas->begin(GFX_SKIP_OUTPUT_BEGIN);
  // PSRAM is not enabled in this build, so plain malloc (32 KB) when absent.
  tilted = (uint16_t *)(psramFound() ? ps_malloc(W * H * 2) : malloc(W * H * 2));
  if (!tilted || !canvas->getFramebuffer()) {
    Serial.println("ERR out of memory for screen buffers");
    for (;;) delay(1000);
  }

  prefs.begin("keypads");
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 1000000); // vendor speed
  scanUnits();
  // Vendor waits >1 s between releasing reset and talking to the panels.
  while (millis() - rstReleased < 1200) delay(10);
  initScreens();
  drawAll();
  for (size_t i = 0; i < screenCount; i++) printCal(i);
}

uint32_t nextScan = 0;

void loop() {
  pollKeys();
  pollKnob();
  handleSerial();
  if (millis() >= nextScan) {
    if (scanUnits()) {
      initScreens();
      drawAll();
    }
    nextScan = millis() + 1000;
  }
}
