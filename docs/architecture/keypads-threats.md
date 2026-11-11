# keypads — threats

**v1** — D2, 2026-11-11

Posture: synthetic-or-public data, no PII, local-only, trusted actors,
explore. Threats are written as **accidents**, not attacks. The new
element is the USB serial command channel between Hammerspoon and the
LilyGO. Its neighbours are the HID output and Hammerspoon's
Accessibility access.

No network, no secrets. The only stored data is the current mode and
profile names (Hammerspoon settings, device NVS).

## STRIDE: serial channel and what it touches

| STRIDE | Threat (accident) | Control |
|---|---|---|
| Spoofing | Another local program opens the LilyGO's serial port, e.g. a serial monitor left running, or the Arduino IDE probing ports. | The device only enters host state on a well-formed `HELLO 1`. A stray program can at most change labels or hold host state, which only affects the owner's own setup. Hammerspoon logs when it can't open the port because it's busy. |
| Tampering | Garbage or partial lines (baud mismatch, a reset mid-line) corrupt state. | Lines ≤128 bytes, `\n`-terminated. The parser drops unknown or malformed lines, out-of-range key numbers, and non-printable label characters. Covered by native tests (AC 9). |
| Repudiation | n/a: single user, local. | — |
| Information disclosure | Key events are visible to whatever reads the serial port. | They are only presses of the pad's own 9 keys and knob, not typed text. Nothing else is logged. |
| Denial of service | Hammerspoon dies, the Mac sleeps or the KVM switches while in host state, leaving the pad dead. | 3 s PING timeout plus port-close detection drop to standalone (AC 4). |
| Denial of service | A key is stuck down after a state change, KVM switch or profile switch mid-press. | The firmware releases all HID keys on every state or profile change and on USB suspend. The Mac side releases Talk/Space on port close (as the old pad did). |
| Elevation | The serial channel becomes a way to type into whatever app is focused. | By design no serial command produces HID output (AC 3). In host state the Mac synthesises keys through Hammerspoon, as the old pad already did. |

## Other accidents

- **Wrong state on the wrong machine.** Standalone WoW keys could be
  typed into a Mac app if Hammerspoon isn't running. Impact: stray
  keystrokes. Key 1 always shows the active profile, so the state is
  visible.
- **Game rules.** Some games prohibit hardware that sends several
  actions per press. The firmware only ever maps one press to one
  keystroke (AC 8). There are no macros, timers or repeats.
- **Bricking while flashing.** The ESP32-S3 ROM bootloader (hold
  BOOT/knob while plugging in) always recovers.

## Out of scope

- A malicious local process using the serial port. Trusted actors and
  local reach: anything that can open the port can already post
  keystrokes on the Mac directly.
- Malicious firmware. The owner builds and flashes it from this repo.

## Supply chain

Hammerspoon (Homebrew cask, signed). PlatformIO platform
`espressif32@6.5.0`, plus Arduino_GFX 1.3.7, pinned in `platformio.ini`.
The LilyGO pin config is copied from the vendor repo with the source
commit recorded.

## Delta v1.4: key-face images over serial (2026-11-11)

What changed: the Mac now sends 128×128 key faces as compressed raw pixels
(`IMG <slot> <bytes>` then `IMD <base64>` lines). The device decompresses
them into RGB565 and draws them. There is no image-format library; the
format is our own PackBits-style run-length encoding of 16-bit pixels.

| STRIDE | Threat (accident) | Control |
|---|---|---|
| Tampering | Truncated, garbled or interleaved image data (Hammerspoon reload mid-send, dropped bytes during a screen draw) | Declared size ≤ 48 KB; base64 checked per line; a new `IMG` aborts an unfinished one; decompression must produce exactly 16384 pixels and never writes past the slot buffer, or the image is dropped and the key keeps its text label. Native tests cover these. RX buffer raised to 32 KB so draws don't drop bytes. |
| DoS | Image flood stalls key handling | Mac paces sends (one queued face per tick) and only resends faces that changed. Device draws at most one dirty key per loop. |
| DoS | Memory exhaustion | Fixed slots (9 keys + 8 mode faces), each one 32 KB buffer in PSRAM, allocated once at boot. |
| Elevation | Image data used to cause key output | Unchanged invariant: no serial command produces HID. Image commands only touch display buffers. |

Re-verified: the HID invariant (no new HID call sites) and the handshake
rule (Mac sends images only after the banner).

**Update (same day): faces are stored in flash by name.** The owner chose
to keep faces on the device. The Mac uploads a face once (`IMG @<name>
<bytes>`), the device checks it by unpacking, then saves the packed bytes
to LittleFS as `/f/<name>` and replies `SAVED <name>`. `FACE <k1-9|m1-8>
<name>` loads a saved face into a slot (`MISSING <name>` if absent). Names
are content hashes made by the Mac.

| STRIDE | Threat (accident) | Control |
|---|---|---|
| Tampering | A face name becomes a file path (`../`, `/`, odd characters) | Names must be 1–16 of `[a-z0-9_-]`, checked in the parser and again in the assembler (native tests). The path is always `/f/` + name. |
| Tampering | A partial or corrupt file after a failed write or power loss | Faces are saved only after unpacking succeeds; a failed write removes the file. Loading unpacks into scratch and changes the slot only on success; otherwise `MISSING`, and the Mac re-uploads. |
| DoS | Flash fills up (many uploads) | Each face ≤ 32,896 bytes; content-hash names mean the same face is stored once. If a write fails the device replies `ERR SAVE` and the key keeps its text label. Clearing old faces is left as a known limitation (about 3 MB holds 100+ faces). |

## Security Review

Posture: synthetic-or-public, no PII, local-only, trusted actors, experiment / explore. Depth: D2 (serial channel plus HID output and Hammerspoon synthesised keys it touches).

**Reviewer**: Security Analyst (Opus)
**Date**: 2026-11-11
**Verdict**: PASS WITH CONDITIONS

### Findings

1. **Medium. Standard: threat-modelling-stride. Category: DoS / state confusion (Mac side).** The stuck-key controls are all on the device or on port close. If the device drops to standalone on PING timeout while the port stays open (for example, Hammerspoon's main thread stalls for more than 3 s, or a `KEY 2 U` line is lost or corrupted), the Mac keeps re-posting Space key-downs every 30 ms for Talk. Nothing ends this. When the device re-sends `HELLO keypads`, the Mac does not reset its state. The same gap can also let standalone HID keys type into the focused Mac app while Hammerspoon still thinks it is in host state.
2. **Medium. Standard: input-validation-at-boundaries. Category: Spoofing (accident).** `303a:1001` is Espressif's default VID/PID, so other ESP32-S3 dev boards on the desk will match it. The spec does not say that the Mac waits for the device banner before writing. As written, `HELLO`/`PING`/`LABEL` could be sent to an unrelated board. Hammerspoon holding the port open will also block `pio run -t upload` (port busy).
3. **Medium. Standard: input-validation-at-boundaries. Category: Tampering.** Parser rules and the AC 9 tests cover only the device. Lines from the device (`KEY`, `KNOB`, `MODE <name>`) are external input to Hammerspoon. Partial lines, out-of-range keys and unknown mode names need the same handling there.
4. **Low. Standard: threat-modelling-stride. Category: Elevation (invariant).** "No serial command causes HID output" is only checked by hand (AC 3). The release-all report sent on entering host state is HID output triggered by serial. It is benign, but it should be named as the only exception. The spec also does not say what happens to a key that is physically held across the host-to-standalone transition. It should not emit a key-down until it is released and pressed again.
5. **Low. Type: Opinion. Category: DoS.** If Talk is held on both pads at once and they share `modes.lua` state, releasing one pad's Talk may stop the other pad's recording. This is functional, so it is noted only.

### Out of scope for current posture

- A malicious local process driving the serial port, and authentication on `HELLO`. Actors are trusted and reach is local; anything that can open the port can already post keystrokes through the Mac. I agree with the threat model's out-of-scope section.
- Signed firmware and secure boot. The owner builds and flashes the firmware.

### Conditions

1. Add Mac-side liveness. The device answers each `PING` with `PONG`. If 3 s pass with no line from the device, or the device sends `HELLO keypads` again, the Mac releases every synthesised key (Talk/Space/Dictation) and resets to the handshake state. Add this to AC 4.
2. Hammerspoon writes nothing to a port until it has read `HELLO keypads 1` from it. It does not rely on VID/PID alone. Record the port-busy-while-flashing behaviour (stop or reload the loader before flashing) in Known limitations.
3. Apply the device parser's rules to the Mac parser: whole lines only, a length cap, key 1–9 and knob values in range, and `MODE <name>` looked up in `modes.lua` with unknown names ignored and logged. Extend AC 9 to cover it.
4. Enforce the invariant in code. Use one HID-send function that refuses in host state, with release-all as the only exception, and add a native test that feeds every protocol line and asserts that no HID report other than release-all is produced. Ignore keys held across a state change until they are released.

### Delta v1.4 review

**Date**: 2026-11-11. Posture unchanged (synthetic-or-public, local-only, trusted actors, explore). Depth: D1 delta on the serial input, checked against the controls named in the delta.

**Verdict**: PASS WITH CONDITIONS

The controls (bounded declared size, per-line base64 check, abort-on-new-`IMG`, exact-16384-pixel decode, fixed PSRAM slots, HID invariant unchanged) fit the accident threats. Gaps are sizing and state rules the delta leaves unstated:

1. **Medium. Standard: input-validation-at-boundaries. Category: Tampering.** The delta caps the declared size at 48 KB but does not name the buffer that holds the compressed bytes. The 32 KB slot holds the decoded face. Name a separate fixed staging buffer of at least the cap, or lower the cap. Worst-case PackBits for 16384 16-bit pixels is about 32.9 KB, so ~33 KB is enough. Reject an `IMD` that would exceed the declared size. Only decode once exactly the declared bytes have arrived.
2. **Low. Category: Tampering.** `MAX_LINE` is 128, so each `IMD` line must carry at most 128 base64 characters. The Mac chunking must match this. Reject a slot outside 0–16. Any other command arriving mid-image must abort the image, or the delta must say that it is processed and the image continues.
3. **Low. Category: Tampering.** Decode into staging and copy to the slot only on success. A failed image must never leave a half-written face on screen.

### Delta v1.4 conditions

1. Name the compressed staging buffer and its size, and align the declared-size cap with it (finding 1). Add a native test for an over-size `IMD`.
2. State the `IMD` line length, the slot range, and the interleaved-command rule. Cover each in native tests (finding 2).
3. Commit a face to its slot only after the decode succeeds (finding 3).

### Delta v1.4 review (update: flash storage)

**Date**: 2026-11-11. Posture unchanged. Depth: D1 delta (face name becomes a LittleFS path; faces persist in flash).

**Verdict**: PASS WITH CONDITIONS

The earlier v1.4 conditions are met. The cap is `MAX_IMG_BYTES` = 32,896 with a fixed staging buffer. `IMD` lines are ≤ 96 characters. Faces are unpacked into scratch and copied to the slot only on success. Native tests cover these.

The path input is sound. `validFaceName` (1–16 characters of `[a-z0-9_-]`) runs before any copy in `readName`, and again in `ImageAssembler::begin`. `facePath` only ever builds `/f/` + name, so `.`, `/` and NUL cannot reach it. `parseFace` bounds `k1-9`/`m1-8` against 1-based arrays sized `MAX_KEYS + 1` / `MAX_MODES + 1`. `loadFace` checks `0 < len <= MAX_IMG_BYTES` before reading into `loadBuf`, and unpacks before it changes the slot. `saveFace` removes a partial file. Commands that arrive mid-upload are handled normally, which is documented in `handleCommand`.

1. **Low. Standard: input-validation-at-boundaries. Category: DoS (accident).** `setup()` does not null-check `bigAlloc` results (`staging`, `loadBuf`, `scratch`, the 17 face buffers). The PSRAM path is fine. Without PSRAM, or in a build with PSRAM disabled, about 650 KB will not fit in internal heap. The first image or `FACE` command then does a `memcpy` into null and the device resets.
2. **Low. Type: Opinion. Category: Tampering.** Opening with `"w"` truncates an existing face before the write. If that write fails, the face is lost and reported as `MISSING`, and the Mac re-uploads it. This is acceptable as documented.

### Delta v1.4 (flash) conditions

1. If any image buffer fails to allocate, disable image commands (`ERR IMG nomem`) and keep text labels. Never dereference a null buffer.
