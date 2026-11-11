#include "protocol.h"

#include <string.h>

namespace proto {

namespace {

bool startsWith(const char *line, size_t len, const char *word, size_t &rest) {
  size_t n = strlen(word);
  if (len < n || memcmp(line, word, n) != 0) return false;
  rest = n;
  return true;
}

bool isPrintable(char c) { return c >= 0x20 && c <= 0x7E; }

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parseLabel(const char *p, size_t len, Command &out) {
  // "<key> <rrggbb> <text>" ; text may be empty
  if (len < 8 || p[0] < '1' || p[0] > '0' + MAX_KEY || p[1] != ' ') return false;
  uint32_t rgb = 0;
  for (size_t i = 2; i < 8; i++) {
    int v = hexVal(p[i]);
    if (v < 0) return false;
    rgb = (rgb << 4) | (uint32_t)v;
  }
  size_t t = 8, n = 0;
  if (len > 8) {
    if (p[8] != ' ') return false;
    t = 9;
  }
  for (size_t i = t; i < len && n < MAX_LABEL; i++)
    if (isPrintable(p[i])) out.text[n++] = p[i]; // anything else is dropped
  out.text[n] = 0;
  out.key = (uint8_t)(p[0] - '0');
  out.rgb = rgb;
  return true;
}

bool parseModes(const char *p, size_t len, Command &out) {
  // "a,b,c" : 1..MAX_MODES names of 1..MAX_MODE_NAME printable chars
  uint8_t count = 0;
  size_t n = 0;
  for (size_t i = 0; i <= len; i++) {
    if (i == len || p[i] == ',') {
      if (n == 0 || count >= MAX_MODES) return false;
      out.modes[count][n] = 0;
      count++;
      n = 0;
      continue;
    }
    if (!isPrintable(p[i]) || n >= MAX_MODE_NAME) return false;
    out.modes[count][n++] = p[i];
  }
  out.modeCount = count;
  return true;
}

int b64Val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// Strict base64: length a multiple of 4, '=' only as trailing padding.
bool decodeBase64(const char *p, size_t len, uint8_t *out, uint8_t &outLen) {
  if (len == 0 || len % 4 != 0 || len > MAX_B64_LINE) return false;
  size_t n = 0;
  for (size_t i = 0; i < len; i += 4) {
    int v[4];
    int pad = 0;
    for (int j = 0; j < 4; j++) {
      char c = p[i + j];
      if (c == '=') {
        if (i + 4 != len || j < 2) return false; // padding only at the very end
        v[j] = 0;
        pad++;
      } else {
        if (pad) return false; // data after padding
        v[j] = b64Val(c);
        if (v[j] < 0) return false;
      }
    }
    uint32_t bits = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
    out[n++] = bits >> 16;
    if (pad < 2) out[n++] = (bits >> 8) & 0xFF;
    if (pad < 1) out[n++] = bits & 0xFF;
  }
  outLen = (uint8_t)n;
  return true;
}

// Reads a face name up to the end or a space. Returns its length, 0 if invalid.
size_t readName(const char *p, size_t len, char *out) {
  size_t n = 0;
  while (n < len && p[n] != ' ') n++;
  if (!validFaceName(p, n)) return 0;
  memcpy(out, p, n);
  out[n] = 0;
  return n;
}

bool parseImgBegin(const char *p, size_t len, Command &out) {
  // "@<name> <size>"
  if (len < 4 || p[0] != '@') return false;
  size_t n = readName(p + 1, len - 1, out.name);
  if (!n || 1 + n >= len || p[1 + n] != ' ') return false;
  uint32_t size = 0;
  size_t digits = 0;
  for (size_t i = 2 + n; i < len; i++, digits++) {
    if (p[i] < '0' || p[i] > '9' || digits >= 6) return false;
    size = size * 10 + (uint32_t)(p[i] - '0');
  }
  if (!digits || size == 0 || size > MAX_IMG_BYTES) return false;
  out.size = size;
  return true;
}

bool parseFace(const char *p, size_t len, Command &out) {
  // "<k|m><index> <name>"
  if (len < 4 || (p[0] != 'k' && p[0] != 'm') || p[1] < '1' || p[1] > '9' || p[2] != ' ')
    return false;
  uint8_t index = (uint8_t)(p[1] - '0');
  if ((p[0] == 'k' && index > MAX_KEY) || (p[0] == 'm' && index > MAX_MODES)) return false;
  size_t n = readName(p + 3, len - 3, out.name);
  if (!n || 3 + n != len) return false;
  out.slotKind = p[0];
  out.slotIndex = index;
  return true;
}

} // namespace

bool parseLine(const char *line, size_t len, Command &out) {
  out = Command();
  if (len > MAX_LINE) return false;
  size_t r;
  if (len == 7 && memcmp(line, "HELLO 1", 7) == 0) {
    out.type = Cmd::Hello;
    return true;
  }
  if (len == 4 && memcmp(line, "PING", 4) == 0) {
    out.type = Cmd::Ping;
    return true;
  }
  if (startsWith(line, len, "LABEL ", r)) {
    if (!parseLabel(line + r, len - r, out)) {
      out = Command();
      return false;
    }
    out.type = Cmd::Label;
    return true;
  }
  if (startsWith(line, len, "IMD ", r)) {
    if (!decodeBase64(line + r, len - r, out.data, out.dataLen)) {
      out = Command();
      return false;
    }
    out.type = Cmd::ImgData;
    return true;
  }
  if (startsWith(line, len, "IMG ", r)) {
    if (!parseImgBegin(line + r, len - r, out)) {
      out = Command();
      return false;
    }
    out.type = Cmd::ImgBegin;
    return true;
  }
  if (startsWith(line, len, "FACE ", r)) {
    if (!parseFace(line + r, len - r, out)) {
      out = Command();
      return false;
    }
    out.type = Cmd::Face;
    return true;
  }
  if (startsWith(line, len, "MODES ", r)) {
    if (!parseModes(line + r, len - r, out)) {
      out = Command();
      return false;
    }
    out.type = Cmd::Modes;
    return true;
  }
  return false;
}

bool validFaceName(const char *name, size_t len) {
  if (len == 0 || len > MAX_FACE_NAME) return false;
  for (size_t i = 0; i < len; i++) {
    char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  return true;
}

bool ImageAssembler::begin(const char *name, uint32_t size) {
  active_ = false;
  size_t n = strlen(name);
  if (size == 0 || size > cap_ || !validFaceName(name, n)) return false;
  memcpy(name_, name, n + 1);
  want_ = size;
  got_ = 0;
  active_ = true;
  return true;
}

bool ImageAssembler::feed(const uint8_t *data, size_t len) {
  if (!active_) return false;
  if (len > want_ - got_) { // more than declared: drop the image
    active_ = false;
    return false;
  }
  memcpy(buf_ + got_, data, len);
  got_ += len;
  if (got_ < want_) return false;
  active_ = false;
  return true;
}

bool unpackFace(const uint8_t *in, size_t len, uint16_t *out, size_t pixels) {
  size_t i = 0, o = 0;
  while (i < len) {
    uint8_t h = in[i++];
    if (h < 128) {
      size_t n = (size_t)h + 1;
      if (n > pixels - o || len - i < n * 2) return false;
      for (size_t k = 0; k < n; k++, i += 2) out[o++] = (uint16_t)(in[i] | (in[i + 1] << 8));
    } else {
      size_t n = (size_t)h - 126;
      if (n > pixels - o || len - i < 2) return false;
      uint16_t px = (uint16_t)(in[i] | (in[i + 1] << 8));
      i += 2;
      for (size_t k = 0; k < n; k++) out[o++] = px;
    }
  }
  return o == pixels;
}

bool LineBuffer::push(char c) {
  if (ready_) {
    len_ = 0;
    ready_ = false;
  }
  if (c == '\r') return false;
  if (c == '\n') {
    if (overflow_) {
      overflow_ = false;
      len_ = 0;
      return false;
    }
    buf_[len_] = 0;
    ready_ = true;
    return true;
  }
  if (overflow_) return false;
  if (len_ >= MAX_LINE) {
    overflow_ = true;
    return false;
  }
  buf_[len_++] = c;
  return false;
}

} // namespace proto
