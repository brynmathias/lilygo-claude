// keypads firmware for the LilyGO T-Keyboard-S3-Pro.
// Design: docs/architecture/keypads.md
//
// This step implements the host state (Mac drives labels and actions over
// USB serial) and the on-device mode picker. Standalone HID profiles come
// next; until then the device sends no HID at all when no Mac is attached.
//
// Hardware facts (I2C write-then-probe, active-low backlight, panel
// offsets, level key reads) are from the spike: see "Spike results".

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Wire.h>
#include <math.h>
#include <soc/usb_serial_jtag_struct.h>

#include "calibration.h"
#include "protocol.h"

// ---------------------------------------------------------------- hardware
// Pins: LilyGO libraries/common/pin_config.h @ cb41b37.
constexpr int PIN_I2C_SDA = 42, PIN_I2C_SCL = 2;
constexpr int PIN_LCD_MOSI = 40, PIN_LCD_SCLK = 41, PIN_LCD_DC = 39, PIN_LCD_RST = 38;
constexpr int PIN_LCD_BL = 1; // active-low
constexpr int PIN_KNOB_A = 4, PIN_KNOB_B = 5, PIN_KNOB_KEY = 0;

constexpr uint8_t REG_LCD_CS = 0x01, REG_KEY_TRIGGER = 0x02;
// Key lights (vendor T-Keyboard-S3-Pro_Drive.h). CONTROL_1 bits 0-5 and
// CONTROL_2 bits 0-7 select LEDs; CONTROL_1 0x80 clears the selection and
// 0x40 shows the colour on the selected LEDs.
constexpr uint8_t REG_LED_MODE = 0x03, REG_LED_BRIGHTNESS = 0x04, REG_LED_HUE_H = 0x05,
                  REG_LED_HUE_L = 0x06, REG_LED_SATURATION = 0x07, REG_LED_CONTROL_1 = 0x08,
                  REG_LED_CONTROL_2 = 0x09, REG_FIRMWARE_VERSION = 0x10;
constexpr uint8_t LED_CTRL_CLEAR = 0x80, LED_CTRL_SHOW = 0x40, LED_SELECT_1 = 0x3F,
                  LED_SELECT_2 = 0xFF;
// The STM32's own LED modes (register 0x03).
constexpr uint8_t LED_MODE_NORMAL = 1, LED_MODE_FREE = 2, LED_MODE_TEST_1 = 3,
                  LED_MODE_TEST_2 = 4;
constexpr uint32_t LIGHT_STEP_MS = 40; // pulse update rate (25 Hz)
constexpr uint8_t MAIN_UNIT = 0x01;
constexpr uint8_t SLOT_BITS[5] = {0b10000, 0b01000, 0b00100, 0b00010, 0b00001};
constexpr uint32_t CS_SETTLE_MS = 20, KEY_READ_WAIT_MS = 20, SPI_HZ = 4500000;
constexpr int16_t W = 128, H = 128;
constexpr size_t MAX_UNITS = 6, MAX_KEYS = 9;

// ---------------------------------------------------------------- timing
constexpr uint32_t HELLO_EVERY_MS = 2000;   // banner while waiting for a Mac
constexpr uint32_t HOST_TIMEOUT_MS = 3000;  // no PING for this long: standalone
constexpr uint32_t PICKER_TIMEOUT_MS = 5000;
constexpr uint32_t UNIT_SCAN_EVERY_MS = 1000;
// USB self-recovery. Behind a powered hub the pad stays on through a Mac
// restart, and the chip's USB serial often stays stuck afterwards (no data
// either way; only a replug fixed it). The bus is "gone" after this long with
// no USB start-of-frame (the host sends one every 1 ms while awake)...
constexpr uint32_t USB_GONE_MS = 1000;
// ...and if no Mac says HELLO this long after it comes back, drop off the bus
// and reboot once, which the Mac sees as a replug.
constexpr uint32_t USB_RECOVER_MS = 15000;
constexpr uint32_t USB_DETACH_MS = 200; // D+ released long enough for the host to notice

class TunableST7735 : public Arduino_ST7735 {
public:
  using Arduino_ST7735::Arduino_ST7735;
  void setOffsets(uint8_t col, uint8_t row) {
    COL_OFFSET1 = COL_OFFSET2 = _xStart = col;
    ROW_OFFSET1 = ROW_OFFSET2 = _yStart = row;
    _currentX = _currentY = _currentW = _currentH = 0xFFFF;
  }
};

Arduino_DataBus *bus = new Arduino_HWSPI(PIN_LCD_DC, -1, PIN_LCD_SCLK, PIN_LCD_MOSI, -1);
TunableST7735 *gfx =
    new TunableST7735(bus, -1, 0, true, W, H, DEFAULT_COL, DEFAULT_ROW, DEFAULT_COL, DEFAULT_ROW);
Arduino_Canvas *canvas = new Arduino_Canvas(W, H, nullptr);
uint16_t *tilted = nullptr;
Preferences prefs;

struct Key {
  uint8_t unit, bit;     // where its screen and switch live
  uint8_t col, row;      // panel calibration
  int8_t tilt;
  bool held = false;     // last read state
  bool ignore = false;   // swallow until released (held across a change)
};

uint8_t units[MAX_UNITS];
size_t unitCount = 0;
Key keys[MAX_KEYS];
size_t keyCount = 0; // 4 (main unit only) or 9 (with expansion)

// ---------------------------------------------------------------- I2C
// The unit controller applies a write only when the next transaction
// arrives, so every write is followed by an empty probe.
bool writeReg(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  bool ok = Wire.endTransmission() == 0;
  Wire.beginTransmission(addr);
  Wire.endTransmission();
  return ok;
}

// LED writes leave the controller a gap, as the vendor demos do. Unit
// controllers with firmware v1 (REG_FIRMWARE_VERSION; our expansion) can't
// set hue: it is OR-ed into the old value (LilyGO issue #7, fixed in v2).
constexpr uint32_t LED_WRITE_GAP_US = 2000;
void ledWrite(uint8_t addr, uint8_t reg, uint8_t value) {
  writeReg(addr, reg, value);
  delayMicroseconds(LED_WRITE_GAP_US);
}

bool readReg(uint8_t addr, uint8_t reg, uint8_t *out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return false;
  delay(KEY_READ_WAIT_MS);
  if (Wire.requestFrom(addr, (uint8_t)1) != 1) return false;
  *out = Wire.read();
  return true;
}

// ---------------------------------------------------------------- state
bool host = false;
uint32_t lastPingMs = 0, lastHelloMs = 0;
// Dimming: the Mac asks (DIM 1 when idle or locked), or no Mac and no key
// press for STANDALONE_DIM_MS (e.g. the Mac is asleep).
constexpr uint32_t STANDALONE_DIM_MS = 60000;
constexpr uint8_t DIM_BACKLIGHT_PERCENT = 8, DIM_LIGHT_BRIGHTNESS = 3;
bool macDim = false, aloneDim = false, dimShown = false;
uint8_t backlightPercent = 100; // set by the Mac (BACKLIGHT)
uint32_t standaloneSinceMs = 0, lastLocalKeyMs = 0;

struct Label {
  char text[proto::MAX_LABEL + 1] = {0};
  uint32_t rgb = 0x202020;
  const uint16_t *face = nullptr; // a key-face image, drawn instead of text
  uint32_t faceGen = 0;           // changes whenever that image changes
  bool operator==(const Label &o) const {
    return rgb == o.rgb && face == o.face && faceGen == o.faceGen && strcmp(text, o.text) == 0;
  }
};

// Key faces. The Mac uploads each face once (IMG @name + IMD lines); it is
// checked by unpacking, then saved to flash as /f/<name> (packed). FACE
// <slot> <name> loads it from flash into a slot. Slots are allocated once
// in PSRAM. Faces are unpacked into `scratch` and copied into a slot only
// if unpacking succeeds (security review v1.4).
struct Face {
  uint16_t *px = nullptr;
  bool valid = false;
  uint32_t gen = 0;
};
Face keyFaces[MAX_KEYS + 1]; // 1-based
Face modeFaces[proto::MAX_MODES + 1];
uint8_t *staging = nullptr; // upload in progress
uint8_t *loadBuf = nullptr; // face being loaded from flash
bool fsOk = false;
bool facesOk = false; // all face buffers allocated; image commands refused otherwise
uint16_t *scratch = nullptr;
proto::ImageAssembler *assembler = nullptr;
uint32_t faceGenCounter = 0;

void setFace(Label &l, const Face &f) {
  if (!f.valid) return;
  l.face = f.px;
  l.faceGen = f.gen;
}

void clearFaces() {
  for (auto &f : keyFaces) f.valid = false;
  for (auto &f : modeFaces) f.valid = false;
  if (assembler) assembler->abort();
}
Label macLabels[MAX_KEYS + 1]; // 1-based, as set by LABEL
Label drawn[MAX_KEYS + 1];
bool drawnValid[MAX_KEYS + 1] = {false};

char modeNames[proto::MAX_MODES][proto::MAX_MODE_NAME + 1];
uint8_t modeCount = 0;
bool picker = false;
uint8_t pickerPage = 0;
uint32_t pickerTouchedMs = 0;
bool knobIgnoreUp = false;

void sendLine(const char *fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  Serial.println(buf);
}

// Anything physically held now is ignored until released, so it can't
// send a down/up in the new state (security condition 4).
void ignoreHeldKeys() {
  for (size_t i = 0; i < keyCount; i++)
    if (keys[i].held) keys[i].ignore = true;
  if (digitalRead(PIN_KNOB_KEY) == LOW) knobIgnoreUp = true;
}

void invalidateAll() {
  for (size_t k = 0; k <= MAX_KEYS; k++) drawnValid[k] = false;
}

// The Mode key is the last key with the expansion (9). The main unit alone
// has no Mode key: all 4 keys go to the Mac, which picks modes from its
// menu bar. Returns 0 (no key) then.
size_t modeKey() { return keyCount >= 9 ? keyCount : 0; }
size_t pickerSlots() { return keyCount > 1 ? keyCount - 1 : 0; }
size_t pickerPages() {
  size_t s = pickerSlots();
  return s ? (modeCount + s - 1) / s : 0;
}

void closePicker() {
  if (!picker) return;
  picker = false;
  invalidateAll();
}

void openPicker() {
  if (!host || !modeCount || !pickerSlots()) return;
  picker = true;
  pickerPage = 0;
  pickerTouchedMs = millis();
  invalidateAll();
}

void enterHost() {
  host = true;
  lastPingMs = millis();
  ignoreHeldKeys();
  closePicker();
  invalidateAll();
}

void enterStandalone() {
  if (!host) return;
  host = false;
  standaloneSinceMs = millis();
  macDim = false; // the Mac can't wake us any more; the standalone timer takes over
  modeCount = 0;
  for (auto &l : macLabels) l = Label();
  clearFaces();
  ignoreHeldKeys();
  closePicker();
  invalidateAll();
}

// What key k (1-based) should show right now.
Label desiredLabel(size_t k) {
  Label l;
  if (!host) {
    if (k == keyCount) {
      strcpy(l.text, "No Mac");
      l.rgb = 0x303030;
    }
    return l;
  }
  if (picker) {
    if (k == modeKey()) {
      strcpy(l.text, pickerPages() > 1 ? "Pick (knob=more)" : "Pick mode");
      l.rgb = 0x404040;
      return l;
    }
    size_t i = pickerPage * pickerSlots() + (k - 1);
    if (i < modeCount) {
      strncpy(l.text, modeNames[i], proto::MAX_LABEL);
      l.rgb = 0x1E3A5F;
      setFace(l, modeFaces[i + 1]);
    }
    return l;
  }
  l = macLabels[k];
  setFace(l, keyFaces[k]);
  return l;
}

// ---------------------------------------------------------------- drawing
void loadCal(Key &k) {
  uint8_t col = DEFAULT_COL, row = DEFAULT_ROW;
  int8_t tilt = 0;
  for (const ScreenCal &c : SCREEN_CAL)
    if (c.unit == k.unit && c.bit == k.bit) {
      col = c.col;
      row = c.row;
      tilt = c.tilt;
    }
  char key[8];
  snprintf(key, sizeof key, "c%02x%02x", k.unit, k.bit); // saved by the spike
  uint32_t v = prefs.getULong(key, (col << 16) | (row << 8) | (uint8_t)(tilt + 128));
  k.col = (v >> 16) & 0xFF;
  k.row = (v >> 8) & 0xFF;
  k.tilt = (int)(v & 0xFF) - 128;
}

uint16_t rgb565(uint32_t rgb) {
  return ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F);
}

bool isLight(uint32_t rgb) {
  uint32_t r = rgb >> 16, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
  return (r * 299 + g * 587 + b * 114) / 1000 > 150;
}

void rotateInto(const uint16_t *src, uint16_t *dst, int tiltDeg, uint16_t bg) {
  if (tiltDeg == 0) {
    memcpy(dst, src, W * H * 2);
    return;
  }
  float a = -tiltDeg * (float)M_PI / 180.0f;
  int32_t c = (int32_t)(cosf(a) * 65536), s = (int32_t)(sinf(a) * 65536);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      int32_t dx = x - W / 2, dy = y - H / 2;
      int32_t sx = ((dx * c - dy * s) >> 16) + W / 2, sy = ((dx * s + dy * c) >> 16) + H / 2;
      dst[y * W + x] = (sx >= 0 && sx < W && sy >= 0 && sy < H) ? src[sy * W + sx] : bg;
    }
}

// Word-wraps `text` into up to 3 lines of at most `cols` characters.
int wrap(const char *text, int cols, char lines[3][proto::MAX_LABEL + 1]) {
  int n = 0;
  const char *p = text;
  while (*p && n < 3) {
    while (*p == ' ') p++;
    int len = strlen(p);
    if (len <= cols) {
      strcpy(lines[n++], p);
      break;
    }
    int cut = cols;
    while (cut > 0 && p[cut] != ' ') cut--;
    if (cut == 0) cut = cols; // one long word: hard break
    memcpy(lines[n], p, cut);
    lines[n][cut] = 0;
    n++;
    p += cut;
  }
  if (*p && n == 3) return 0; // doesn't fit
  return n;
}

void drawKey(size_t k, const Label &l) {
  const Key &key = keys[k - 1];
  uint16_t bg = rgb565(l.rgb), fg = isLight(l.rgb) ? BLACK : WHITE;
  if (l.face) {
    memcpy(canvas->getFramebuffer(), l.face, W * H * 2);
    rotateInto(canvas->getFramebuffer(), tilted, key.tilt, l.face[0]);
    writeReg(key.unit, REG_LCD_CS, key.bit);
    delay(CS_SETTLE_MS);
    gfx->setOffsets(key.col, key.row);
    gfx->draw16bitRGBBitmap(0, 0, tilted, W, H);
    writeReg(key.unit, REG_LCD_CS, 0);
    return;
  }
  canvas->fillScreen(bg);
  if (k == modeKey() && host && !picker) {
    canvas->setTextSize(1);
    canvas->setTextColor(fg);
    canvas->setCursor(4, 4);
    canvas->print("MODE");
  }
  if (l.text[0]) {
    // Largest text size at which the label fits in up to 3 lines.
    for (int size = 4; size >= 1; size--) {
      char lines[3][proto::MAX_LABEL + 1];
      int n = wrap(l.text, (W - 8) / (6 * size), lines);
      if (!n || n * 8 * size + (n - 1) * 4 > H - 16) continue;
      int y = (H - (n * 8 * size + (n - 1) * 4)) / 2;
      canvas->setTextSize(size);
      canvas->setTextColor(fg);
      for (int i = 0; i < n; i++) {
        int w = strlen(lines[i]) * 6 * size;
        canvas->setCursor((W - w) / 2, y);
        canvas->print(lines[i]);
        y += 8 * size + 4;
      }
      break;
    }
  }
  rotateInto(canvas->getFramebuffer(), tilted, key.tilt, bg);
  writeReg(key.unit, REG_LCD_CS, key.bit);
  delay(CS_SETTLE_MS);
  gfx->setOffsets(key.col, key.row);
  gfx->draw16bitRGBBitmap(0, 0, tilted, W, H);
  writeReg(key.unit, REG_LCD_CS, 0);
}

// Draws at most one out-of-date key per call, so key polling keeps going
// while a batch of labels arrives.
void drawNextDirty() {
  for (size_t k = 1; k <= keyCount; k++) {
    Label want = desiredLabel(k);
    if (drawnValid[k] && drawn[k] == want) continue;
    drawKey(k, want);
    drawn[k] = want;
    drawnValid[k] = true;
    return;
  }
}

// ---------------------------------------------------------------- units
void selectUnits(uint8_t mask) {
  for (size_t i = 0; i < unitCount; i++) {
    writeReg(units[i], REG_LCD_CS, mask);
    delay(10);
  }
}

void initScreens() {
  selectUnits(0b11111);
  gfx->setOffsets(DEFAULT_COL, DEFAULT_ROW);
  gfx->begin(SPI_HZ);
  gfx->fillScreen(BLACK);
  delay(100);
  selectUnits(0);
  invalidateAll();
}

// Returns true if the set of units changed.
bool scanUnits() {
  uint8_t found[MAX_UNITS];
  size_t n = 0;
  for (uint8_t a = 1; a < 127 && n < MAX_UNITS; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) found[n++] = a;
  }
  if (n == unitCount && memcmp(found, units, n) == 0) return false;
  memcpy(units, found, n);
  unitCount = n;
  keyCount = 0;
  for (size_t u = 0; u < unitCount && keyCount < MAX_KEYS; u++) {
    int slots = units[u] == MAIN_UNIT ? 4 : 5; // main unit's 5th slot is the knob
    for (int s = 0; s < slots && keyCount < MAX_KEYS; s++) {
      Key &k = keys[keyCount++];
      k = Key();
      k.unit = units[u];
      k.bit = SLOT_BITS[s];
      loadCal(k);
    }
  }
  return true;
}

// Unit addresses, so the Mac can tell which expansion units are attached.
void sendUnits() {
  char buf[48] = "UNITS";
  for (size_t i = 0; i < unitCount; i++)
    snprintf(buf + strlen(buf), sizeof buf - strlen(buf), " 0x%02X", units[i]);
  sendLine("%s", buf);
}

void onLayoutChange() {
  ignoreHeldKeys();
  closePicker();
  initScreens();
  if (host) {
    sendLine("LAYOUT %u", (unsigned)keyCount);
    sendUnits();
  }
}

// ---------------------------------------------------------------- input
void keyDown(size_t k) {
  lastLocalKeyMs = millis();
  if (k == modeKey()) { // handled here, never sent
    if (picker) closePicker();
    else openPicker();
    keys[k - 1].ignore = true;
    return;
  }
  if (picker) {
    size_t i = pickerPage * pickerSlots() + (k - 1);
    if (i < modeCount) sendLine("MODE %s", modeNames[i]);
    closePicker();
    keys[k - 1].ignore = true; // its release belongs to the picker
    return;
  }
  if (host) sendLine("KEY %u D", (unsigned)k);
}

void keyUp(size_t k) {
  if (host) sendLine("KEY %u U", (unsigned)k);
}

void pollKeys() {
  for (size_t u = 0; u < unitCount; u++) {
    uint8_t v;
    if (!readReg(units[u], REG_KEY_TRIGGER, &v)) continue;
    for (size_t i = 0; i < keyCount; i++) {
      Key &key = keys[i];
      if (key.unit != units[u]) continue;
      bool now = v & key.bit;
      if (now == key.held) continue;
      key.held = now;
      if (now) {
        if (!key.ignore) keyDown(i + 1);
      } else if (key.ignore) {
        key.ignore = false;
      } else {
        keyUp(i + 1);
      }
    }
  }
}

volatile int32_t knobSteps = 0;
volatile uint8_t knobPrev = 0;
bool knobHeld = false;

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
    if (picker) {
      size_t pages = pickerPages();
      if (pages > 1) {
        pickerPage = (pickerPage + pages + (steps > 0 ? 1 : -1)) % pages;
        pickerTouchedMs = millis();
        invalidateAll();
      }
    } else if (host) {
      sendLine("KNOB %+ld", (long)steps);
    }
  }
  bool down = digitalRead(PIN_KNOB_KEY) == LOW;
  if (down == knobHeld) return;
  knobHeld = down;
  if (down) {
    if (picker) { // knob press cancels the picker
      closePicker();
      knobIgnoreUp = true;
    } else if (host && !knobIgnoreUp) {
      sendLine("KNOB P D");
    }
  } else if (knobIgnoreUp) {
    knobIgnoreUp = false;
  } else if (host) {
    sendLine("KNOB P U");
  }
}

// ---------------------------------------------------------------- serial
proto::LineBuffer rx;

String facePath(const char *name) { return String("/f/") + name; } // name is [a-z0-9_-]

bool saveFace(const char *name, const uint8_t *data, size_t len) {
  if (!fsOk) return false;
  File f = LittleFS.open(facePath(name), "w");
  if (!f) return false;
  bool ok = f.write(data, len) == len;
  f.close();
  if (!ok) LittleFS.remove(facePath(name)); // never leave a partial file
  return ok;
}

// Loads a saved face into a slot. The slot only changes on success.
bool loadFace(const char *name, Face &slot) {
  if (!fsOk || !LittleFS.exists(facePath(name))) return false; // no error log noise
  File f = LittleFS.open(facePath(name), "r");
  if (!f) return false;
  size_t len = f.size();
  bool ok = len > 0 && len <= proto::MAX_IMG_BYTES && f.read(loadBuf, len) == len &&
            proto::unpackFace(loadBuf, len, scratch, proto::FACE_PIXELS);
  f.close();
  if (!ok) return false;
  memcpy(slot.px, scratch, proto::FACE_PIXELS * 2);
  slot.valid = true;
  slot.gen = ++faceGenCounter;
  return true;
}

// ---------------------------------------------------------------- lights
struct Light {
  uint8_t mode = proto::LIGHT_EFFECT_2; // the vendor demo's flowing colours
  uint8_t brightness = 15;
  uint16_t hue = 20; // warm orange until the Mac says otherwise
  uint8_t saturation = 80;
  uint16_t pulseMs = 0;
};
Light light;
uint32_t nextLightStep = 0;
uint8_t lightShown = 255; // brightness last written in colour mode

bool dimmed() { return macDim || aloneDim; }
uint8_t lightBrightness() {
  return dimmed() && light.brightness > DIM_LIGHT_BRIGHTNESS ? DIM_LIGHT_BRIGHTNESS : light.brightness;
}

void setBacklight(uint8_t percent) {
  ledcWrite(1, 255 - (uint32_t)percent * 255 / 100); // active-low
}

void applyBacklight() {
  bool dim = dimmed() && backlightPercent > DIM_BACKLIGHT_PERCENT;
  setBacklight(dim ? DIM_BACKLIGHT_PERCENT : backlightPercent);
}

void showBrightness(uint8_t b) {
  for (size_t i = 0; i < unitCount; i++) {
    ledWrite(units[i], REG_LED_BRIGHTNESS, b);
    ledWrite(units[i], REG_LED_CONTROL_1, LED_SELECT_1);
    ledWrite(units[i], REG_LED_CONTROL_2, LED_SELECT_2);
    ledWrite(units[i], REG_LED_CONTROL_1, LED_CTRL_SHOW);
  }
  lightShown = b;
}

// Writes the whole light state to every unit (on change, or a new unit).
void applyLight() {
  for (size_t i = 0; i < unitCount; i++) {
    uint8_t u = units[i];
    switch (light.mode) {
    case proto::LIGHT_NORMAL:
    case proto::LIGHT_EFFECT_1:
    case proto::LIGHT_EFFECT_2: {
      uint8_t m = light.mode == proto::LIGHT_NORMAL     ? LED_MODE_NORMAL
                  : light.mode == proto::LIGHT_EFFECT_1 ? LED_MODE_TEST_1
                                                        : LED_MODE_TEST_2;
      ledWrite(u, REG_LED_BRIGHTNESS, lightBrightness());
      ledWrite(u, REG_LED_MODE, m);
      break;
    }
    default: // off, or a colour
      ledWrite(u, REG_LED_MODE, LED_MODE_FREE);
      ledWrite(u, REG_LED_HUE_H, (uint8_t)(light.hue >> 8));
      ledWrite(u, REG_LED_HUE_L, (uint8_t)light.hue);
      ledWrite(u, REG_LED_SATURATION, light.saturation);
      ledWrite(u, REG_LED_CONTROL_1, LED_CTRL_CLEAR);
      break;
    }
  }
  lightShown = 255;
  if (light.mode == proto::LIGHT_OFF) showBrightness(0);
  else if (light.mode == proto::LIGHT_COLOUR) showBrightness(lightBrightness());
}

// Applies a dim/undim when the wanted state changed.
void updateDim() {
  if (dimmed() == dimShown) return;
  dimShown = dimmed();
  applyBacklight();
  applyLight();
}

// Breathing: brightness follows a smooth curve from 10% to 100% of the set
// brightness over pulseMs. Only colour mode pulses.
void stepLight(uint32_t now) {
  if (light.mode != proto::LIGHT_COLOUR || !light.pulseMs || now < nextLightStep) return;
  nextLightStep = now + LIGHT_STEP_MS;
  float phase = (float)(now % light.pulseMs) / light.pulseMs;    // 0..1
  float level = 0.55f - 0.45f * cosf(phase * 2.0f * (float)PI); // 0.1..1
  uint8_t b = (uint8_t)(lightBrightness() * level + 0.5f);
  if (b != lightShown) showBrightness(b);
}

void handleCommand(const proto::Command &c) {
  switch (c.type) {
  case proto::Cmd::Hello:
    enterHost();
    sendLine("LAYOUT %u", (unsigned)keyCount);
    sendUnits();
    sendLine("INFO psram=%u free=%u faces=%s fs=%s", (unsigned)ESP.getPsramSize(),
             (unsigned)ESP.getFreePsram(), facesOk ? "ok" : "off", fsOk ? "ok" : "off");
    for (size_t i = 0; i < unitCount; i++) {
      uint8_t v = 0;
      if (readReg(units[i], REG_FIRMWARE_VERSION, &v)) sendLine("UNITFW 0x%02x %#x", units[i], v);
    }
    break;
  case proto::Cmd::Ping:
    if (!host) break; // must say HELLO first
    lastPingMs = millis();
    sendLine("PONG");
    break;
  case proto::Cmd::Modes:
    if (!host) break;
    modeCount = c.modeCount;
    memcpy(modeNames, c.modes, sizeof modeNames);
    if (picker) invalidateAll();
    break;
  case proto::Cmd::Label:
    if (!host) break;
    strcpy(macLabels[c.key].text, c.text);
    macLabels[c.key].rgb = c.rgb;
    keyFaces[c.key].valid = false; // an IMG may follow
    break;
  case proto::Cmd::ImgBegin:
    if (!facesOk) {
      sendLine("ERR IMG %s nomem", c.name);
      break;
    }
    // Other commands may arrive mid-upload and are handled normally; only a
    // new IMG (or leaving host state) abandons an unfinished one.
    if (!host) break;
    if (!assembler->begin(c.name, c.size)) sendLine("ERR IMG %s size", c.name);
    break;
  case proto::Cmd::ImgData:
    if (!host || !facesOk || !assembler->active()) break;
    if (!assembler->feed(c.data, c.dataLen)) {
      if (!assembler->active()) sendLine("ERR IMG overflow");
      break;
    }
    if (!proto::unpackFace(assembler->data(), assembler->size(), scratch, proto::FACE_PIXELS))
      sendLine("ERR IMG %s unpack", assembler->name());
    else if (saveFace(assembler->name(), assembler->data(), assembler->size()))
      sendLine("SAVED %s", assembler->name());
    else
      sendLine("ERR SAVE %s", assembler->name());
    break;
  case proto::Cmd::Face: {
    if (!host) break;
    if (!facesOk) { // keys keep their text labels
      sendLine("ERR FACE %s nomem", c.name);
      break;
    }
    Face &f = c.slotKind == 'k' ? keyFaces[c.slotIndex] : modeFaces[c.slotIndex];
    if (!loadFace(c.name, f)) sendLine("MISSING %s", c.name);
    break;
  }
  case proto::Cmd::Light:
    light.mode = c.lightMode;
    light.brightness = c.lightBrightness;
    light.hue = c.lightHue;
    light.saturation = c.lightSaturation;
    light.pulseMs = c.lightPulseMs;
    applyLight();
    break;
  case proto::Cmd::Backlight:
    backlightPercent = c.backlight;
    applyBacklight();
    break;
  case proto::Cmd::Dim:
    macDim = c.dim;
    updateDim();
    break;
  default:
    break;
  }
}

void pollSerial() {
  while (Serial.available()) {
    if (!rx.push((char)Serial.read())) continue;
    proto::Command c;
    if (!proto::parseLine(rx.data(), rx.size(), c)) continue;
    // Any valid line from the Mac proves it is alive, not just PING: a
    // PING can sit behind a face upload while the flash write runs.
    if (host) lastPingMs = millis();
    handleCommand(c);
  }
}

// ---------------------------------------------------------------- main
void *bigAlloc(size_t n) { return psramFound() ? ps_malloc(n) : malloc(n); }

// ---------------------------------------------------------------- USB recovery
// Watches the start-of-frame counter: it only advances while a host drives
// the bus. Triggered by bus state alone; no serial command can cause it.
uint16_t lastSof = 0;
uint32_t lastSofChangeMs = 0, usbReturnedMs = 0;
bool usbGone = false;

void reconnectUsb() {
  USB_SERIAL_JTAG.conf0.pad_pull_override = 1;
  USB_SERIAL_JTAG.conf0.dp_pullup = 0; // the host sees an unplug
  delay(USB_DETACH_MS);
  ESP.restart();
}

void watchUsb(uint32_t now) {
  uint16_t f = USB_SERIAL_JTAG.fram_num.sof_frame_index;
  if (f != lastSof) {
    if (usbGone) usbReturnedMs = now ? now : 1;
    usbGone = false;
    lastSof = f;
    lastSofChangeMs = now;
  } else if (now - lastSofChangeMs > USB_GONE_MS) {
    usbGone = true; // seen unchanged by a sample, not inferred from a long stall
  }
  if (host) usbReturnedMs = 0;
  if (usbReturnedMs && now - usbReturnedMs > USB_RECOVER_MS) reconnectUsb();
}

void setup() {
  // Undo reconnectUsb()'s override, which can survive a software reset.
  USB_SERIAL_JTAG.conf0.pad_pull_override = 0;
  // A screen draw blocks for ~80 ms; the default 256-byte RX buffer would
  // drop image data meanwhile.
  Serial.setRxBufferSize(32768);
  Serial.begin(115200);
  // Never block for long on output: if the Mac is busy writing to us and not
  // reading, waiting here deadlocks both sides. Lines may drop; the protocol
  // tolerates that (PINGs repeat, the Mac re-sends faces).
  Serial.setTxTimeoutMs(10);

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
  setBacklight(100);

  canvas->begin(GFX_SKIP_OUTPUT_BEGIN);
  tilted = (uint16_t *)bigAlloc(W * H * 2);
  staging = (uint8_t *)bigAlloc(proto::MAX_IMG_BYTES);
  loadBuf = (uint8_t *)bigAlloc(proto::MAX_IMG_BYTES);
  fsOk = LittleFS.begin(true); // formats the data partition on first use
  if (fsOk && !LittleFS.exists("/f")) LittleFS.mkdir("/f");
  scratch = (uint16_t *)bigAlloc(proto::FACE_PIXELS * 2);
  assembler = new proto::ImageAssembler(staging, proto::MAX_IMG_BYTES);
  for (auto &f : keyFaces) f.px = (uint16_t *)bigAlloc(proto::FACE_PIXELS * 2);
  for (auto &f : modeFaces) f.px = (uint16_t *)bigAlloc(proto::FACE_PIXELS * 2);
  // Only after every buffer above is allocated.
  facesOk = staging && loadBuf && scratch && tilted;
  for (auto &f : keyFaces) facesOk = facesOk && f.px;
  for (auto &f : modeFaces) facesOk = facesOk && f.px;

  prefs.begin("keypads", true);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 1000000);
  scanUnits();
  while (millis() - rstReleased < 1200) delay(10); // panels need >1 s after reset
  initScreens();
  applyLight();
  lastSof = USB_SERIAL_JTAG.fram_num.sof_frame_index;
  lastSofChangeMs = millis(); // a bus already up at boot is not a "return"
}

uint32_t nextUnitScan = 0;

void loop() {
  pollSerial();
  pollKeys();
  pollKnob();
  // Read the clock after polling: polling updates lastPingMs, and an older
  // `now` would make now - lastPingMs wrap to a huge value.
  uint32_t now = millis();

  if (host && now - lastPingMs > HOST_TIMEOUT_MS) enterStandalone();
  if (!host && now - lastHelloMs > HELLO_EVERY_MS) {
    sendLine("HELLO keypads 1 keys=%u knob=1", (unsigned)keyCount);
    lastHelloMs = now;
  }
  if (picker && now - pickerTouchedMs > PICKER_TIMEOUT_MS) closePicker();
  if (now >= nextUnitScan) {
    if (scanUnits()) {
      onLayoutChange();
      applyLight(); // a newly attached unit starts dark
    }
    nextUnitScan = now + UNIT_SCAN_EVERY_MS;
  }
  {
    uint32_t quietSince = standaloneSinceMs;
    if ((int32_t)(lastLocalKeyMs - quietSince) > 0) quietSince = lastLocalKeyMs;
    aloneDim = !host && now - quietSince > STANDALONE_DIM_MS;
    updateDim();
  }
  stepLight(now);
  watchUsb(now);
  drawNextDirty();
}
