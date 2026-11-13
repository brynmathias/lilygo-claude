// Native tests for the Mac -> device line parser (AC 9).
// Run: pio test -e native  (from services/keypads/src/firmware)
#include <string.h>
#include <string>
#include <unity.h>

#include "protocol.h"

using namespace proto;

static bool parse(const char *s, Command &c) { return parseLine(s, strlen(s), c); }

void setUp() {}
void tearDown() {}

void test_hello_and_ping() {
  Command c;
  TEST_ASSERT_TRUE(parse("HELLO 1", c));
  TEST_ASSERT(c.type == Cmd::Hello);
  TEST_ASSERT_TRUE(parse("PING", c));
  TEST_ASSERT(c.type == Cmd::Ping);
  TEST_ASSERT_FALSE(parse("HELLO 2", c));
  TEST_ASSERT_FALSE(parse("HELLO 1 ", c));
  TEST_ASSERT_FALSE(parse("PINGX", c));
  TEST_ASSERT(c.type == Cmd::None);
}

void test_label_ok() {
  Command c;
  TEST_ASSERT_TRUE(parse("LABEL 3 D9785A Esc", c));
  TEST_ASSERT(c.type == Cmd::Label);
  TEST_ASSERT_EQUAL(3, c.key);
  TEST_ASSERT_EQUAL_HEX32(0xD9785A, c.rgb);
  TEST_ASSERT_EQUAL_STRING("Esc", c.text);
}

void test_label_empty_text() {
  Command c;
  TEST_ASSERT_TRUE(parse("LABEL 9 000000", c));
  TEST_ASSERT_EQUAL_STRING("", c.text);
  TEST_ASSERT_TRUE(parse("LABEL 9 000000 ", c));
  TEST_ASSERT_EQUAL_STRING("", c.text);
}

void test_label_rejects_bad_key_and_colour() {
  Command c;
  TEST_ASSERT_FALSE(parse("LABEL 0 FFFFFF x", c));
  TEST_ASSERT_FALSE(parse("LABEL : FFFFFF x", c));
  TEST_ASSERT_FALSE(parse("LABEL 10 FFFFFF x", c));
  TEST_ASSERT_FALSE(parse("LABEL 2 FFFFFG x", c));
  TEST_ASSERT_FALSE(parse("LABEL 2 FFFFF", c));
  TEST_ASSERT_FALSE(parse("LABEL 2 FFFFFFx", c));
  TEST_ASSERT(c.type == Cmd::None);
}

void test_label_drops_non_ascii_and_truncates() {
  Command c;
  TEST_ASSERT_TRUE(parse("LABEL 2 FFFFFF \xE2\x87\xA7Tab\x01", c)); // "⇧Tab" + control char
  TEST_ASSERT_EQUAL_STRING("Tab", c.text);
  TEST_ASSERT_TRUE(parse("LABEL 2 FFFFFF abcdefghijklmnopqrstuvwxyz", c));
  TEST_ASSERT_EQUAL(MAX_LABEL, strlen(c.text));
}

void test_modes() {
  Command c;
  TEST_ASSERT_TRUE(parse("MODES Claude,iTerm,Browser", c));
  TEST_ASSERT(c.type == Cmd::Modes);
  TEST_ASSERT_EQUAL(3, c.modeCount);
  TEST_ASSERT_EQUAL_STRING("iTerm", c.modes[1]);
  TEST_ASSERT_FALSE(parse("MODES ", c));
  TEST_ASSERT_FALSE(parse("MODES a,,b", c));
  TEST_ASSERT_FALSE(parse("MODES a,b,c,d,e,f,g,h,i", c)); // 9 > MAX_MODES
  TEST_ASSERT_FALSE(parse("MODES abcdefghijklmnopq", c)); // 17 chars
  TEST_ASSERT_FALSE(parse("MODES a\x01", c));
}

void test_unknown_and_oversized() {
  Command c;
  TEST_ASSERT_FALSE(parse("", c));
  TEST_ASSERT_FALSE(parse("TYPE hello", c));
  std::string big = "LABEL 2 FFFFFF " + std::string(200, 'x');
  TEST_ASSERT_FALSE(parseLine(big.c_str(), big.size(), c));
}

void test_line_buffer() {
  LineBuffer b;
  const char *in = "PING\r\nHELLO 1\n";
  int lines = 0;
  for (const char *p = in; *p; p++)
    if (b.push(*p)) {
      lines++;
      if (lines == 1) TEST_ASSERT_EQUAL_STRING("PING", b.data());
      if (lines == 2) TEST_ASSERT_EQUAL_STRING("HELLO 1", b.data());
    }
  TEST_ASSERT_EQUAL(2, lines);
}

void test_line_buffer_drops_oversized_line_whole() {
  LineBuffer b;
  for (int i = 0; i < 300; i++) TEST_ASSERT_FALSE(b.push('x'));
  TEST_ASSERT_FALSE(b.push('\n')); // the long line is dropped
  const char *next = "PING\n";
  bool got = false;
  for (const char *p = next; *p; p++) got = b.push(*p);
  TEST_ASSERT_TRUE(got);
  TEST_ASSERT_EQUAL_STRING("PING", b.data());
}

// ---- images (security review delta v1.4, conditions 1-3)

void test_img_begin() {
  Command c;
  TEST_ASSERT_TRUE(parse("IMG @mic 100", c));
  TEST_ASSERT(c.type == Cmd::ImgBegin);
  TEST_ASSERT_EQUAL_STRING("mic", c.name);
  TEST_ASSERT_EQUAL(100, c.size);
  TEST_ASSERT_TRUE(parse("IMG @a1b2c3d4e5f60718 32896", c)); // 16-char name, max size
  TEST_ASSERT_FALSE(parse("IMG @a1b2c3d4e5f607189 10", c)); // 17 chars
  TEST_ASSERT_FALSE(parse("IMG mic 10", c));                // no @
  TEST_ASSERT_FALSE(parse("IMG @ 10", c));
  TEST_ASSERT_FALSE(parse("IMG @mic 0", c));
  TEST_ASSERT_FALSE(parse("IMG @mic 32897", c)); // over the worst case
  TEST_ASSERT_FALSE(parse("IMG @mic 12a", c));
  TEST_ASSERT_FALSE(parse("IMG @mic 9999999", c));
  TEST_ASSERT_FALSE(parse("IMG @mic", c));
}

// Face names become file names in flash: no path characters allowed.
void test_face_names_are_safe_paths() {
  Command c;
  TEST_ASSERT_FALSE(parse("IMG @../x 10", c));
  TEST_ASSERT_FALSE(parse("IMG @a/b 10", c));
  TEST_ASSERT_FALSE(parse("IMG @Mic 10", c)); // upper case
  TEST_ASSERT_FALSE(parse("IMG @a.b 10", c));
  TEST_ASSERT_FALSE(parse("FACE k1 ../../etc", c));
  TEST_ASSERT_FALSE(parse("FACE k1 a b", c));
  TEST_ASSERT_TRUE(validFaceName("ok_name-9", 9));
  TEST_ASSERT_FALSE(validFaceName("", 0));
}

void test_face() {
  Command c;
  TEST_ASSERT_TRUE(parse("FACE k1 mic", c));
  TEST_ASSERT(c.type == Cmd::Face);
  TEST_ASSERT_EQUAL('k', c.slotKind);
  TEST_ASSERT_EQUAL(1, c.slotIndex);
  TEST_ASSERT_EQUAL_STRING("mic", c.name);
  TEST_ASSERT_TRUE(parse("FACE m8 robot", c));
  TEST_ASSERT_FALSE(parse("FACE m9 robot", c)); // only 8 mode slots
  TEST_ASSERT_FALSE(parse("FACE k0 mic", c));
  TEST_ASSERT_FALSE(parse("FACE x1 mic", c));
  TEST_ASSERT_FALSE(parse("FACE k1 ", c));
}

void test_imd_base64() {
  Command c;
  TEST_ASSERT_TRUE(parse("IMD AQID", c)); // 01 02 03
  TEST_ASSERT(c.type == Cmd::ImgData);
  TEST_ASSERT_EQUAL(3, c.dataLen);
  TEST_ASSERT_EQUAL(3, c.data[2]);
  TEST_ASSERT_TRUE(parse("IMD AQI=", c));
  TEST_ASSERT_EQUAL(2, c.dataLen);
  TEST_ASSERT_TRUE(parse("IMD AQ==", c));
  TEST_ASSERT_EQUAL(1, c.dataLen);
  TEST_ASSERT_FALSE(parse("IMD AQI", c));   // not a multiple of 4
  TEST_ASSERT_FALSE(parse("IMD A=QI", c));  // padding in the middle
  TEST_ASSERT_FALSE(parse("IMD AQ==AQID", c));
  TEST_ASSERT_FALSE(parse("IMD AQ!D", c));
  std::string tooLong = "IMD " + std::string(100, 'A'); // > 96 base64 chars
  TEST_ASSERT_FALSE(parseLine(tooLong.c_str(), tooLong.size(), c));
}

void test_assembler_exact_and_overflow() {
  uint8_t buf[8];
  ImageAssembler a(buf, sizeof buf);
  uint8_t d[5] = {1, 2, 3, 4, 5};
  TEST_ASSERT_FALSE(a.begin("x", 9)); // larger than the staging buffer
  TEST_ASSERT_FALSE(a.begin("../x", 6)); // unsafe name
  TEST_ASSERT_TRUE(a.begin("x", 6));
  TEST_ASSERT_FALSE(a.feed(d, 4));
  TEST_ASSERT_TRUE(a.active());
  TEST_ASSERT_FALSE(a.feed(d, 4)); // 8 > 6 declared: dropped
  TEST_ASSERT_FALSE(a.active());
  TEST_ASSERT_TRUE(a.begin("y", 6));
  TEST_ASSERT_FALSE(a.feed(d, 3));
  TEST_ASSERT_TRUE(a.feed(d, 3)); // exactly 6: complete
  TEST_ASSERT_EQUAL(6, a.size());
  TEST_ASSERT_FALSE(a.feed(d, 1)); // nothing active after completion
}

void test_assembler_new_begin_aborts_old() {
  uint8_t buf[8];
  ImageAssembler a(buf, sizeof buf);
  uint8_t d[4] = {9, 9, 9, 9};
  TEST_ASSERT_TRUE(a.begin("first", 8));
  a.feed(d, 4);
  TEST_ASSERT_TRUE(a.begin("second", 4)); // restarts: old bytes discarded
  TEST_ASSERT_TRUE(a.feed(d, 4));
  TEST_ASSERT_EQUAL_STRING("second", a.name());
}

void test_unpack_runs_and_literals() {
  // run of 3 x 0x1234, then 2 literals 0xAAAA 0xBBBB  -> 5 pixels
  const uint8_t in[] = {129, 0x34, 0x12, 1, 0xAA, 0xAA, 0xBB, 0xBB};
  uint16_t out[5];
  TEST_ASSERT_TRUE(unpackFace(in, sizeof in, out, 5));
  TEST_ASSERT_EQUAL_HEX16(0x1234, out[2]);
  TEST_ASSERT_EQUAL_HEX16(0xBBBB, out[4]);
}

void test_unpack_rejects_wrong_counts() {
  uint16_t out[4] = {7, 7, 7, 7};
  const uint8_t tooMany[] = {130, 1, 0}; // run of 4 into 3 pixels
  TEST_ASSERT_FALSE(unpackFace(tooMany, sizeof tooMany, out, 3));
  const uint8_t tooFew[] = {128, 1, 0}; // run of 2 into 4 pixels
  TEST_ASSERT_FALSE(unpackFace(tooFew, sizeof tooFew, out, 4));
  const uint8_t truncated[] = {3, 1, 0, 2, 0}; // says 4 literals, has 2
  TEST_ASSERT_FALSE(unpackFace(truncated, sizeof truncated, out, 4));
  const uint8_t noPixel[] = {200}; // run header with no pixel
  TEST_ASSERT_FALSE(unpackFace(noPixel, sizeof noPixel, out, 4));
}

void test_unpack_never_writes_past_end() {
  uint16_t out[5] = {0, 0, 0, 0xBEEF, 0xBEEF};
  const uint8_t in[] = {255, 1, 0}; // run of 129 into a 3-pixel buffer
  TEST_ASSERT_FALSE(unpackFace(in, sizeof in, out, 3));
  TEST_ASSERT_EQUAL_HEX16(0xBEEF, out[3]);
}

void test_unpack_full_face_worst_case_fits_cap() {
  static uint8_t in[MAX_IMG_BYTES];
  size_t n = 0;
  for (size_t px = 0; px < FACE_PIXELS; px += 128) {
    in[n++] = 127; // 128 literals
    for (int k = 0; k < 128; k++) { in[n++] = 0x11; in[n++] = 0x22; }
  }
  TEST_ASSERT_EQUAL(MAX_IMG_BYTES, n);
  static uint16_t out[FACE_PIXELS];
  TEST_ASSERT_TRUE(unpackFace(in, n, out, FACE_PIXELS));
}

void test_light_valid() {
  Command c;
  TEST_ASSERT_TRUE(parse("LIGHT 2 60 25 80 1500", c));
  TEST_ASSERT_EQUAL(Cmd::Light, c.type);
  TEST_ASSERT_EQUAL(2, c.lightMode);
  TEST_ASSERT_EQUAL(60, c.lightBrightness);
  TEST_ASSERT_EQUAL(25, c.lightHue);
  TEST_ASSERT_EQUAL(80, c.lightSaturation);
  TEST_ASSERT_EQUAL(1500, c.lightPulseMs);
  TEST_ASSERT_TRUE(parse("LIGHT 4 100 360 100 9999", c));
  TEST_ASSERT_TRUE(parse("LIGHT 0 0 0 0 0", c));
}

void test_light_rejects_bad() {
  Command c;
  const char *bad[] = {
      "LIGHT 5 60 25 80 0",    // mode out of range
      "LIGHT 2 101 25 80 0",   // brightness
      "LIGHT 2 60 361 80 0",   // hue
      "LIGHT 2 60 25 101 0",   // saturation
      "LIGHT 2 60 25 80 10000", // pulse
      "LIGHT 2 60 25 80",      // too few
      "LIGHT 2 60 25 80 0 1",  // too many
      "LIGHT 2 60 25 80 0 ",   // trailing space
      "LIGHT 2  60 25 80 0",   // double space
      "LIGHT 2 6x 25 80 0",    // not a number
      "LIGHT 2 60 25 80 -1",
      "LIGHT 2 60 25 80 123456",   // more than 5 digits
      "LIGHT ",
  };
  for (const char *b : bad) {
    TEST_ASSERT_FALSE_MESSAGE(parse(b, c), b);
    TEST_ASSERT_EQUAL(Cmd::None, c.type);
  }
}

void test_dim() {
  Command c;
  TEST_ASSERT_TRUE(parse("DIM 1", c));
  TEST_ASSERT_EQUAL(Cmd::Dim, c.type);
  TEST_ASSERT_TRUE(c.dim);
  TEST_ASSERT_TRUE(parse("DIM 0", c));
  TEST_ASSERT_FALSE(c.dim);
  const char *bad[] = {"DIM", "DIM ", "DIM 2", "DIM 10", "DIM 1 ", "DIMM 1"};
  for (const char *b : bad) {
    TEST_ASSERT_FALSE_MESSAGE(parse(b, c), b);
    TEST_ASSERT_EQUAL(Cmd::None, c.type);
  }
}

void test_backlight() {
  Command c;
  TEST_ASSERT_TRUE(parse("BACKLIGHT 40", c));
  TEST_ASSERT_EQUAL(Cmd::Backlight, c.type);
  TEST_ASSERT_EQUAL(40, c.backlight);
  TEST_ASSERT_TRUE(parse("BACKLIGHT 100", c));
  TEST_ASSERT_TRUE(parse("BACKLIGHT 0", c));
  const char *bad[] = {"BACKLIGHT", "BACKLIGHT ", "BACKLIGHT 101", "BACKLIGHT 4 ", "BACKLIGHT x", "BACKLIGHT 1 2"};
  for (const char *b : bad) {
    TEST_ASSERT_FALSE_MESSAGE(parse(b, c), b);
    TEST_ASSERT_EQUAL(Cmd::None, c.type);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_hello_and_ping);
  RUN_TEST(test_label_ok);
  RUN_TEST(test_label_empty_text);
  RUN_TEST(test_label_rejects_bad_key_and_colour);
  RUN_TEST(test_label_drops_non_ascii_and_truncates);
  RUN_TEST(test_modes);
  RUN_TEST(test_unknown_and_oversized);
  RUN_TEST(test_line_buffer);
  RUN_TEST(test_line_buffer_drops_oversized_line_whole);
  RUN_TEST(test_img_begin);
  RUN_TEST(test_face_names_are_safe_paths);
  RUN_TEST(test_face);
  RUN_TEST(test_imd_base64);
  RUN_TEST(test_assembler_exact_and_overflow);
  RUN_TEST(test_assembler_new_begin_aborts_old);
  RUN_TEST(test_unpack_runs_and_literals);
  RUN_TEST(test_unpack_rejects_wrong_counts);
  RUN_TEST(test_unpack_never_writes_past_end);
  RUN_TEST(test_unpack_full_face_worst_case_fits_cap);
  RUN_TEST(test_light_valid);
  RUN_TEST(test_light_rejects_bad);
  RUN_TEST(test_dim);
  RUN_TEST(test_backlight);
  return UNITY_END();
}
