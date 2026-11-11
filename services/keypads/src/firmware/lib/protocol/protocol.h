// Mac -> device line protocol (docs/architecture/keypads.md, "Serial protocol").
// Pure C++ with no Arduino dependency, so it runs in native tests.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace proto {

constexpr size_t MAX_LINE = 128;   // bytes, excluding '\n'
constexpr size_t MAX_LABEL = 24;   // printable ASCII characters
constexpr size_t MAX_MODES = 8;
constexpr size_t MAX_MODE_NAME = 16;
constexpr uint8_t MAX_KEY = 9;
constexpr size_t MAX_B64_LINE = 96;     // base64 chars per IMD line
constexpr size_t MAX_IMD_BYTES = 72;    // = MAX_B64_LINE / 4 * 3
constexpr size_t FACE_W = 128, FACE_H = 128, FACE_PIXELS = FACE_W * FACE_H;
// Worst case for a packed face: every pixel a literal, one header per 128.
constexpr size_t MAX_IMG_BYTES = FACE_PIXELS * 2 + (FACE_PIXELS + 127) / 128; // 32896

constexpr size_t MAX_FACE_NAME = 16; // [a-z0-9_-], also the file name in flash

enum class Cmd { None, Hello, Ping, Modes, Label, ImgBegin, ImgData, Face };

struct Command {
  Cmd type = Cmd::None;
  // Label
  uint8_t key = 0;
  uint32_t rgb = 0; // 0xRRGGBB
  char text[MAX_LABEL + 1] = {0};
  // Modes
  uint8_t modeCount = 0;
  char modes[MAX_MODES][MAX_MODE_NAME + 1] = {{0}};
  // ImgBegin: upload face `name`; size = packed bytes that follow in
  // ImgData lines. Face: show face `name` on slot 'k' (key, 1..MAX_KEY) or
  // 'm' (mode picker entry, 1..MAX_MODES).
  char name[MAX_FACE_NAME + 1] = {0};
  uint32_t size = 0;
  char slotKind = 0;
  uint8_t slotIndex = 0;
  // ImgData
  uint8_t data[MAX_IMD_BYTES] = {0};
  uint8_t dataLen = 0;
};

// Parses one complete line (no '\n'). Returns false, and leaves `out.type`
// as None, for anything unknown or malformed. Never reads past `len`.
bool parseLine(const char *line, size_t len, Command &out);

// Collects one image's packed bytes into a caller-owned buffer.
class ImageAssembler {
public:
  ImageAssembler(uint8_t *buf, size_t capacity) : buf_(buf), cap_(capacity) {}
  // Starts a new image (aborting any unfinished one).
  bool begin(const char *name, uint32_t size);
  // Adds bytes. Returns true when the image just completed. Overflow aborts.
  bool feed(const uint8_t *data, size_t len);
  void abort() { active_ = false; }
  bool active() const { return active_; }
  const char *name() const { return name_; }
  const uint8_t *data() const { return buf_; }
  size_t size() const { return got_; }

private:
  uint8_t *buf_;
  size_t cap_;
  bool active_ = false;
  char name_[MAX_FACE_NAME + 1] = {0};
  size_t want_ = 0, got_ = 0;
};

// Unpacks a face: a sequence of packets, each a header byte h then
//   h < 128:  h + 1 literal pixels (2 bytes each, little-endian RGB565)
//   h >= 128: one pixel repeated h - 126 times (2..129)
// Returns true only if it produces exactly `pixels` pixels and consumes all
// input. Never writes past `pixels`.
bool unpackFace(const uint8_t *in, size_t len, uint16_t *out, size_t pixels);

// True if `name` is 1..MAX_FACE_NAME of [a-z0-9_-] (safe as a file name).
bool validFaceName(const char *name, size_t len);

// Collects bytes into lines. A line longer than MAX_LINE is dropped whole,
// up to and including its '\n'.
class LineBuffer {
public:
  // Returns true when `c` completes a line; the line is then in data()/size()
  // until the next push().
  bool push(char c);
  const char *data() const { return buf_; }
  size_t size() const { return len_; }

private:
  char buf_[MAX_LINE + 1] = {0};
  size_t len_ = 0;
  bool overflow_ = false;
  bool ready_ = false;
};

} // namespace proto
