-- keypads: LilyGO T-Keyboard-S3-Pro (USB 303a:1001) over USB serial.
-- Design: docs/architecture/keypads.md ("LilyGO: two states", "Serial protocol").
--
-- Keys (1-based, main unit 1–4, expansion 5–9):
--   1 Talk: tap = Enter (or the mode's `tap`), hold = push-to-talk
--   2–3 the mode's `pad4` roles; 2–7 its `pad9` roles (with the expansion)
--   8 knob mode: nav / volume / brightness (with the expansion)
--   9 (with the expansion): Mode, handled on the device (opens the picker,
--     sends MODE <name>). Without the expansion there is no Mode key: key 4
--     is the mode's, and modes are picked from the menu bar.
-- Knob: turn = knob mode (nav = mode up/down), press = select.

local modes, faces = ... -- passed in by the loader (init.lua)
local log = hs.logger.new("lilygo", "info")

local SETTINGS_MODE = "keypads.lilygo.mode"
local SETTINGS_KNOB = "keypads.lilygo.knob"
local TALK_HOLD_SECONDS = 0.25
local PING_SECONDS = 1
local LIVENESS_SECONDS = 3   -- no line from the device for this long: reset
local BANNER_WAIT_SECONDS = 5
local SCAN_SECONDS = 2
local MAX_LINE = 128
local KNOB_KEY = 8
local TALK_KEY = 1

local KEY_BG = "2A2A2A"
local BLANK_BG = "101010"
local TALKING_BG = "C0392B"

local S = {
  port = nil, portName = nil, buf = "", host = false, lastRx = 0,
  keys = 4, mode = 1, knob = 1,
  talkHeld = false, talkRecording = false, talkTimer = nil,
  recorder = modes.newRecorder(),
  tried = {},        -- port names that never sent a banner
  paused = false,
  -- Key faces (emoji images). The device keeps faces in flash by name; we
  -- only upload one when the device answers FACE with MISSING.
  sentLabel = {},    -- key -> last LABEL line sent
  sentFace = {},     -- slot ("k3", "m1") -> face name last sent
  wantFace = {},     -- slot -> face name it should show
  faceSource = {},   -- face name -> { emoji, bg }
  uploadQueue = {},  -- face names to upload, oldest first
  outLines = {},     -- upload lines waiting to be sent, a slice per tick
  queued = {},       -- face name -> true while queued or uploading
}

local function mode() return modes.MODES[S.mode] end
-- The knob's modes: the shared ones, plus "light" (LilyGO only, added below).
local KNOBS = {}
for _, k in ipairs(modes.KNOB_MODES) do KNOBS[#KNOBS + 1] = k end
local function knob() return KNOBS[S.knob] end

local function hex(c)
  return string.format("%02X%02X%02X", math.floor(c.red * 255), math.floor(c.green * 255), math.floor(c.blue * 255))
end

local function send(line)
  if S.port and S.port:isOpen() then S.port:sendData(line .. "\n") end
end

-- Menu bar item: the LilyGO's mode at a glance, e.g. "⌨️ 🤖 CLAUDE ↕️",
-- with 🎤 while talking. Hidden while the LilyGO isn't connected.
-- Clicking it lists the modes.
local setMode -- defined below; the menu needs it
local function updateMenu()
  if not S.menu then
    S.menu = hs.menubar.new()
    S.menu:setMenu(function()
      local items = { { title = "LilyGO: " .. (S.host and "connected" or "not connected"), disabled = true } }
      for i, m in ipairs(modes.MODES) do
        items[#items + 1] = { title = m.label, checked = (i == S.mode), fn = function() setMode(i) end }
      end
      return items
    end)
  end
  if not S.host then -- only show the item while the LilyGO is connected
    S.menu:removeFromMenuBar()
    return
  end
  S.menu:returnToMenuBar()
  S.menu:setTitle("⌨️ " .. mode().label .. " " .. knob().icon .. (S.talkRecording and " 🎤" or ""))
end

-- What a key does in the current mode and layout: role name or nil.
local function roleFor(k)
  local m = mode()
  if S.keys >= 9 and k == S.keys then return nil end -- Mode key: the device owns it
  if k == TALK_KEY then return "talk" end
  if S.keys >= 9 then
    if k == KNOB_KEY then return "knobmode" end
    return (m.pad9 or {})[k]
  end
  return (m.pad4 or {})[k]
end

local function labelFor(role)
  local m = mode()
  local labels = m.labels or {}
  if role == "talk" then
    return S.talkRecording and "Listening" or (labels.talk or "Talk")
  elseif role == "clear" then
    return labels.clear or "Clear"
  elseif role == "knobmode" then
    return knob().label and knob().label() or knob().text -- text, not an emoji
  end
  return labels[role] or role
end

local function resetFaces()
  S.sentLabel, S.sentFace, S.wantFace, S.uploadQueue, S.queued = {}, {}, {}, {}, {}
  S.outLines = {}
end

-- LABEL is the text fallback; the device clears that key's face when it
-- gets one, so only send it when it changes.
local function sendLabel(k, bg, text)
  local line = string.format("LABEL %d %s%s", k, bg, text and (" " .. text) or "")
  if S.sentLabel[k] == line then return end
  send(line)
  S.sentLabel[k] = line
  S.sentFace["k" .. k] = nil
end

local function setFace(slot, content, bg, isText)
  if not content then return end
  local name = faces.name(content, bg, isText)
  S.faceSource[name] = { content, bg, isText }
  S.wantFace[slot] = name
  if S.sentFace[slot] == name then return end
  send("FACE " .. slot .. " " .. name)
  S.sentFace[slot] = name
end

local function iconFor(role)
  if role == "knobmode" then return nil end -- a text face instead (see pushLabel)
  return (mode().icons or {})[role] or modes.ROLE_ICONS[role]
end

local function pushLabel(k)
  if not S.host then return end
  if k == S.keys then -- Mode key
    local bg = hex(mode().color)
    sendLabel(k, bg, mode().short)
    setFace("k" .. k, mode().icon, bg)
    return
  end
  local role = roleFor(k)
  if not role then
    sendLabel(k, BLANK_BG)
    return
  end
  local bg = (role == "talk" and S.talkRecording) and TALKING_BG or KEY_BG
  sendLabel(k, bg, labelFor(role))
  if role == "knobmode" then
    setFace("k" .. k, labelFor(role), bg, true) -- "Scroll" / "Volume" / "Brightness"
  else
    setFace("k" .. k, iconFor(role), bg)
  end
end

-- Key lights: the controllers' own flowing-colour effect (effect 2, as in
-- LilyGO's demo). Colour mode (2) works on the main unit only so far: the
-- expansion keeps its start-up colour. keypads.lilygo.light(...) overrides
-- this until the next mode change.
-- Light patterns, chosen with the knob's "Pattern" mode (saved). Colour
-- mode (LIGHT 2) is left out: the expansion ignores colour changes.
local PATTERNS = {
  { mode = 4, name = "Flow" },   -- LilyGO demo's flowing colours
  { mode = 3, name = "Flash" },  -- controller effect 1
  { mode = 0, name = "Off" },
}
local SETTINGS_PATTERN = "keypads.lilygo.pattern"
S.pattern = math.tointeger(hs.settings.get(SETTINGS_PATTERN)) or 1
if not PATTERNS[S.pattern] then S.pattern = 1 end
-- Pad brightness level 1–10, set with the knob's "Light" mode: screens at
-- level × 10 %, key lights at level × 3 (of 100).
local SETTINGS_LEVEL = "keypads.lilygo.level"
local LEVEL_DEFAULT, LEVEL_MAX = 5, 10
local LEVEL_SCREEN, LEVEL_LIGHT = 10, 3
S.level = math.tointeger(hs.settings.get(SETTINGS_LEVEL)) or LEVEL_DEFAULT
if S.level < 1 or S.level > LEVEL_MAX then S.level = LEVEL_DEFAULT end

local function hueSat(c)
  local r, g, b = c.red, c.green, c.blue
  local max, min = math.max(r, g, b), math.min(r, g, b)
  local d = max - min
  if d == 0 then return 0, 0 end
  local h
  if max == r then h = ((g - b) / d) % 6
  elseif max == g then h = (b - r) / d + 2
  else h = (r - g) / d + 4 end
  return math.floor(h * 60 + 0.5) % 360, math.floor(d / max * 100 + 0.5)
end

local function sendLight()
  local h, sat = hueSat(mode().color)
  send(string.format("LIGHT %d %d %d %d %d", PATTERNS[S.pattern].mode, S.level * LEVEL_LIGHT, h, sat, 0))
  send("BACKLIGHT " .. S.level * LEVEL_SCREEN)
end

local function setLevel(step)
  local level = math.max(1, math.min(LEVEL_MAX, S.level + step))
  if level == S.level then return end
  S.level = level
  hs.settings.set(SETTINGS_LEVEL, level)
  sendLight()
  pushLabel(KNOB_KEY)
end

KNOBS[#KNOBS + 1] = {
  name = "light", icon = "💡", text = "Light",
  label = function() return "Light " .. S.level end,
  ccw = function() setLevel(-1) end, cw = function() setLevel(1) end,
}

local function stepPattern(step)
  S.pattern = (S.pattern - 1 + step) % #PATTERNS + 1
  hs.settings.set(SETTINGS_PATTERN, S.pattern)
  sendLight()
  pushLabel(KNOB_KEY)
end

KNOBS[#KNOBS + 1] = {
  name = "pattern", icon = "🌈", text = "Pattern",
  label = function() return PATTERNS[S.pattern].name end,
  ccw = function() stepPattern(-1) end, cw = function() stepPattern(1) end,
}

-- "All screens": every Mac display and the pad together. Uses BetterDisplay's
-- command line (Settings > Integration must allow it), since the brightness
-- key only changes the focused display. Clicks are coalesced: one
-- BetterDisplay call runs at a time with the steps that piled up meanwhile.
local BETTERDISPLAY = "/Applications/BetterDisplay.app/Contents/MacOS/BetterDisplay"
local DISPLAY_STEP_PERCENT = 10
S.displaySteps, S.displayTask = 0, nil

local function runDisplaySteps()
  if S.displayTask or S.displaySteps == 0 then return end
  local pct = S.displaySteps * DISPLAY_STEP_PERCENT
  S.displaySteps = 0
  S.displayTask = hs.task.new(BETTERDISPLAY, function(code, _, err)
    S.displayTask = nil
    if code ~= 0 then log.w("BetterDisplay brightness failed: " .. tostring(err)) end
    runDisplaySteps()
  end, { "set", "-type=Display", string.format("-brightness=%d%%", pct), "-offset" })
  S.displayTask:start()
end

local function allScreens(step)
  S.displaySteps = S.displaySteps + step
  runDisplaySteps()
  setLevel(step)
end

KNOBS[#KNOBS + 1] = {
  name = "all", icon = "🔆", text = "All Screens",
  ccw = function() allScreens(-1) end, cw = function() allScreens(1) end,
}

local function pushAll()
  if not S.host then return end
  sendLight()
  local names = {}
  for _, m in ipairs(modes.MODES) do names[#names + 1] = m.short end
  send("MODES " .. table.concat(names, ","))
  for i, m in ipairs(modes.MODES) do setFace("m" .. i, m.icon, hex(m.color)) end
  for k = 1, S.keys do pushLabel(k) end
end

-- Sends queued upload lines a slice at a time, so this thread keeps
-- reading from the device between slices (serial writes block, and a
-- 40 KB burst deadlocked with the device's own writes).
local LINES_PER_TICK = 24
local function uploadNext()
  if not S.host then return end
  if #S.outLines > 0 then
    local n = math.min(LINES_PER_TICK, #S.outLines)
    local chunk = {}
    for i = 1, n do chunk[i] = S.outLines[i] end
    send(table.concat(chunk, "\n"))
    table.move(S.outLines, n + 1, #S.outLines + n, 1)
    return
  end
  -- Rendering a face blocks for ~250 ms, once per new face.
  local name = table.remove(S.uploadQueue, 1)
  if not name then return end
  local src = S.faceSource[name]
  if not src then S.queued[name] = nil; return end
  local packed = faces.render(src[1], src[2], src[3])
  local b64 = hs.base64.encode(packed)
  S.outLines[#S.outLines + 1] = string.format("IMG @%s %d", name, #packed)
  for i = 1, #b64, 96 do S.outLines[#S.outLines + 1] = "IMD " .. b64:sub(i, i + 95) end
  log.i(string.format("uploaded face %s (%s, %d bytes)", name, src[1], #packed))
end

local function onSaved(name)
  S.queued[name] = nil
  for slot, want in pairs(S.wantFace) do
    if want == name then
      S.sentFace[slot] = nil
      local src = S.faceSource[name]
      setFace(slot, src[1], src[2], src[3])
    end
  end
end

setMode = function(i)
  S.mode = i
  hs.settings.set(SETTINGS_MODE, mode().name)
  pushAll()
  updateMenu()
end

-- Talk ---------------------------------------------------------------------

local function talkReset()
  if S.talkTimer then S.talkTimer:stop(); S.talkTimer = nil end
  S.talkHeld, S.talkRecording = false, false
  S.recorder.stop()
end

local function talkDown()
  if S.recorder.active() and not S.talkHeld then -- a release went missing
    talkReset()
    pushLabel(TALK_KEY)
    return
  end
  S.talkHeld, S.talkRecording = true, false
  S.talkTimer = hs.timer.doAfter(TALK_HOLD_SECONDS, function()
    S.talkTimer = nil
    S.talkRecording = true
    S.recorder.start(mode())
    pushLabel(TALK_KEY)
    updateMenu()
  end)
end

local function talkUp()
  if not S.talkHeld then return end
  S.talkHeld = false
  if S.talkTimer then S.talkTimer:stop(); S.talkTimer = nil end
  if S.talkRecording then
    S.talkRecording = false
    S.recorder.stop()
    pushLabel(TALK_KEY)
    updateMenu()
  else
    (mode().tap or modes.tap({}, "return"))()
  end
end

-- Idle dimming -------------------------------------------------------------
-- Dims the pad's screens and lights while the Mac is idle (no input for
-- IDLE_SECONDS, from the keyboard, mouse or this pad) or locked/asleep.
-- Wakes within a tick (PING_SECONDS) of any input.
local IDLE_SECONDS = 300
S.lastPadUse = hs.timer.secondsSinceEpoch()
S.locked, S.screensOff, S.dimSent = false, false, nil

local function updateDim()
  if not S.host then return end
  local now = hs.timer.secondsSinceEpoch()
  local idle = S.locked or S.screensOff
    or (hs.host.idleTime() > IDLE_SECONDS and now - S.lastPadUse > IDLE_SECONDS)
  if idle ~= S.dimSent then
    send("DIM " .. (idle and "1" or "0"))
    S.dimSent = idle
  end
end

local W = hs.caffeinate.watcher
S.sleepWatcher = W.new(function(ev)
  if ev == W.screensDidLock then S.locked = true
  elseif ev == W.screensDidUnlock then S.locked = false
  elseif ev == W.screensDidSleep or ev == W.screensaverDidStart then S.screensOff = true
  elseif ev == W.screensDidWake or ev == W.screensaverDidStop then S.screensOff = false
  else return end
  updateDim()
end):start()

-- Device events ------------------------------------------------------------

local function keyDown(k)
  local role = roleFor(k)
  if role == "talk" then return talkDown() end
  if role == "clear" then return (mode().clear or modes.clearText)() end
  if role == "knobmode" then
    S.knob = S.knob % #KNOBS + 1
    hs.settings.set(SETTINGS_KNOB, knob().name)
    updateMenu()
    return pushLabel(KNOB_KEY)
  end
  if role and mode()[role] then mode()[role]() end
end

local function keyUp(k)
  if roleFor(k) == "talk" then talkUp() end
end

local function knobTurn(steps)
  local dir = steps > 0 and "cw" or "ccw"
  for _ = 1, math.min(math.abs(steps), 20) do
    local kn = knob()
    if kn[dir] then kn[dir]()
    else mode()[dir == "ccw" and "up" or "down"]() end
  end
end

local function lostDevice(why)
  if S.host then log.i("device lost: " .. why) end
  S.host = false
  talkReset()
  updateMenu()
end

-- Handles one complete line from the device. Anything unexpected is
-- ignored (the ROM bootloader and crash dumps also print here).
S.rxLog = {} -- last lines from the device, for keypads.lilygo.recent()
local function onLine(line)
  if #line > MAX_LINE then return end
  S.rxLog[#S.rxLog + 1] = line
  if #S.rxLog > 40 then table.remove(S.rxLog, 1) end
  local keys = line:match("^HELLO keypads 1 keys=(%d) knob=1$")
  if keys then
    if S.host then lostDevice("device restarted") end
    S.keys = tonumber(keys)
    S.host = true
    resetFaces()
    send("HELLO 1")
    S.dimSent = nil -- the device starts undimmed; tell it the current state
    pushAll()
    updateDim()
    updateMenu()
    log.i("handshake ok, " .. S.keys .. " keys")
    return
  end
  if not S.host then return end -- nothing else counts before the banner
  if line:match("^KEY ") or line:match("^KNOB ") then
    S.lastPadUse = hs.timer.secondsSinceEpoch()
    updateDim()
  end
  local k, dir = line:match("^KEY (%d) ([DU])$")
  if k then
    k = tonumber(k)
    if k < 1 or k > S.keys then return end
    if S.keys >= 9 and k == S.keys then return end -- the Mode key is never sent
    if dir == "D" then keyDown(k) else keyUp(k) end
    return
  end
  local steps = line:match("^KNOB ([+-]%d%d?)$")
  if steps then return knobTurn(tonumber(steps)) end
  if line == "KNOB P D" then return (knob().press or mode().select)() end
  local name = line:match("^MODE (.+)$")
  if name then
    local i = modes.findMode("short", name)
    if i then setMode(i) else log.w("unknown mode from device: " .. name) end
    return
  end
  local missing = line:match("^MISSING ([a-z0-9_-]+)$")
  if missing then
    if S.faceSource[missing] and not S.queued[missing] then
      S.queued[missing] = true
      S.uploadQueue[#S.uploadQueue + 1] = missing
    end
    return
  end
  local saved = line:match("^SAVED ([a-z0-9_-]+)$")
  if saved then return onSaved(saved) end
  if line:match("^ERR ") then
    log.w("device: " .. line)
    local name = line:match("^ERR %u+ @?([a-z0-9_-]+)")
    if name then S.queued[name] = nil end
    return
  end
  local units = line:match("^UNITS ([0-9a-fA-Fx ]+)$")
  if units then
    if units ~= S.units then log.i("units: " .. units) end
    S.units = units
    return
  end
  local n = line:match("^LAYOUT (%d)$")
  if n then
    S.keys = tonumber(n)
    talkReset()
    resetFaces()
    pushAll()
  end
end

-- Port handling --------------------------------------------------------------

local function closePort()
  local p = S.port
  S.port, S.portName, S.buf = nil, nil, "" -- first, so a "closed" callback is a no-op
  if p then p:close() end
  lostDevice("port closed")
end

local function onSerial(_, kind, message)
  if kind == "received" then
    S.lastRx = hs.timer.secondsSinceEpoch()
    S.buf = S.buf .. message
    if #S.buf > 4 * MAX_LINE and not S.buf:find("\n") then S.buf = "" end
    local lines, lastBanner = {}, 0
    for line in S.buf:gmatch("([^\n]*)\n") do
      line = line:gsub("\r$", "")
      lines[#lines + 1] = line
      if line:match("^HELLO keypads") then lastBanner = #lines end
    end
    -- A backlog can hold several banners; handshake once, on the last.
    for i, line in ipairs(lines) do
      if not (line:match("^HELLO keypads") and i ~= lastBanner) then onLine(line) end
    end
    S.buf = S.buf:match("[^\n]*$")
  elseif (kind == "removed" or kind == "closed" or kind == "error") and S.port then
    log.i("port " .. kind)
    closePort()
  end
end

local function tryPorts()
  if S.paused or S.port then return end
  local present = {}
  for _, name in ipairs(hs.serial.availablePortNames()) do
    present[name] = true
    if name:match("usbmodem") and not S.tried[name] and not S.port then
      local p = hs.serial.newFromName(name)
      if p then
        p:baudRate(115200)
        p:callback(onSerial)
        if p:open() then
          S.port, S.portName, S.buf, S.lastRx = p, name, "", hs.timer.secondsSinceEpoch()
          log.i("opened " .. name .. ", waiting for banner")
          -- Not ours (or not ready) if no banner arrives: close, skip it.
          hs.timer.doAfter(BANNER_WAIT_SECONDS, function()
            if S.port == p and not S.host then
              log.i(name .. " sent no banner; ignoring it until it reappears")
              S.tried[name] = true
              closePort()
            end
          end)
        end
      end
    end
  end
  for name in pairs(S.tried) do
    if not present[name] then S.tried[name] = nil end -- unplugged: try again next time
  end
end

local function tick()
  if S.paused then return end
  if not S.port then return tryPorts() end
  if S.host then
    if hs.timer.secondsSinceEpoch() - S.lastRx > LIVENESS_SECONDS then
      lostDevice("no reply for " .. LIVENESS_SECONDS .. " s")
    else
      send("PING")
      updateDim()
    end
  end
end

-- Restore saved modes.
local savedMode = hs.settings.get(SETTINGS_MODE)
S.mode = modes.findMode("name", savedMode) or 1
local savedKnob = hs.settings.get(SETTINGS_KNOB)
for i, k in ipairs(KNOBS) do
  if k.name == savedKnob then S.knob = i end
end

updateMenu()
S.pingTimer = hs.timer.doEvery(PING_SECONDS, tick)
S.scanTimer = hs.timer.doEvery(SCAN_SECONDS, tryPorts)
S.uploadTimer = hs.timer.doEvery(0.02, uploadNext)
S.usbWatcher = hs.usb.watcher.new(function(ev)
  if ev.vendorID == 0x303a and ev.productID == 0x1001 and ev.eventType == "added" then
    hs.timer.doAfter(1, tryPorts)
  end
end):start()
hs.timer.doAfter(0.5, tryPorts) -- deferred, so a reload via `hs -c` returns first

-- Free the port so `pio run -t upload` can flash, then take it back.
function S.pause()
  S.paused = true
  closePort()
  return "paused: serial port released"
end

-- Deferred: opening the port inside an `hs -c` call left hs.ipc
-- "already recursing" and Hammerspoon unresponsive to the CLI.
function S.resume()
  S.paused = false
  S.tried = {}
  hs.timer.doAfter(0.1, tryPorts)
  return "resuming"
end

-- Try the lights by hand: mode 0 off, 1 normal, 2 colour, 3/4 the
-- controller's own effects; brightness 0-100, hue 0-360, saturation 0-100,
-- pulse period in ms (0 = steady).
function S.light(lmode, brightness, hue, sat, pulseMs)
  local line = string.format("LIGHT %d %d %d %d %d", lmode, brightness or 30, hue or 0, sat or 100, pulseMs or 0)
  send(line)
  return line
end

function S.recent() return table.concat(S.rxLog, "\n") end

function S.status()
  return string.format("port=%s host=%s keys=%d units=%s mode=%s knob=%s", tostring(S.portName),
    tostring(S.host), S.keys, tostring(S.units), mode().name, knob().name)
end

return S
