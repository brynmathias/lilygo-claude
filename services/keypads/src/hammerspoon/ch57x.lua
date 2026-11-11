-- keypads: CH57x 6-key + knob macro pad (USB 1189:8890).
-- Moved from min-claude-keyboard/services/minikeyboard/init.lua; behaviour is
-- unchanged, except that keys and the menu bar item are only active while
-- the pad is plugged in. Mode actions come from modes.lua (shared with the
-- LilyGO). Flash the pad once with ../ch57x/keyboard.yaml.
-- Design: docs/architecture/keypads.md
--
-- Top row 1 2 3, bottom row 4 5 6, knob on the side. The pad sends F13–F20:
--   1 F13 Talk (tap = Enter, hold = push-to-talk)   2 F18 Yes
--   3 ctrl-alt-F16 No                               4 F16 Clear
--   5 F17 mode-specific A                           6 ctrl-alt-F19 Mode
--   knob: F19 / F20 turn, ctrl-alt-F20 press

local modes = ... -- passed in by the loader (init.lua)

local VENDOR, PRODUCT = 0x1189, 0x8890
local SETTINGS_KEY = "minikeyboard.mode" -- same keys as before: modes carry over
local KNOB_SETTINGS_KEY = "minikeyboard.knob"
local SHOW_LEGEND = true -- menu bar shows what buttons 1–5 do in the current mode

local MODES, KNOB_MODES = modes.MODES, modes.KNOB_MODES

-- Button number → mode while in SELECT (double-tap Mode).
local SELECT_KEYS = {
  { button = 1, mode = "claude" },
  { button = 2, mode = "iterm" },
  { button = 3, mode = "browser" },
  { button = 4, mode = "apps" },
  { button = 5, mode = "meet" },
}

local state = {
  index = 1, knob = 1, menu = nil, hotkeys = {}, lastModeTap = nil, selecting = false,
  recorder = modes.newRecorder(), enabled = false,
}

local function currentMode() return MODES[state.index] end
local function currentKnob() return KNOB_MODES[state.knob] end

local setMode -- defined below; the menu needs it

-- Mode indicator lives in the menu bar. Clicking it lists the modes.
local function drawMarker()
  if not state.menu then
    state.menu = hs.menubar.new(false) -- shown only while the pad is plugged in
    state.menu:setMenu(function()
      local items = {}
      for i, m in ipairs(MODES) do
        items[#items + 1] = { title = m.label, checked = (i == state.index), fn = function() setMode(i) end }
      end
      return items
    end)
  end
  if state.selecting then
    local parts = {}
    for _, k in ipairs(SELECT_KEYS) do
      local _, m = modes.findMode("name", k.mode)
      if m then parts[#parts + 1] = k.button .. "=" .. m.label:gsub("^%S+ ", "") end
    end
    state.menu:setTitle("🔀 " .. table.concat(parts, "  ") .. "  knob=KNOB")
    return
  end
  if state.knobSelecting then
    local parts = {}
    for i, k in ipairs(KNOB_MODES) do
      local item = k.icon .. " " .. k.name
      parts[#parts + 1] = (i == state.knob) and ("[" .. item .. "]") or item
    end
    state.menu:setTitle("🎛 " .. table.concat(parts, "  "))
    return
  end
  local mode = currentMode()
  local title = mode.label .. " " .. currentKnob().icon .. (state.dictating and " 🎤" or "")
  if SHOW_LEGEND then
    local keys = { [1] = "Talk", [4] = "Clear" } -- same in every mode
    for b, name in pairs(mode.keys or {}) do keys[b] = name end
    local parts = {}
    for b = 1, 5 do parts[#parts + 1] = b .. "·" .. (keys[b] or "–") end
    title = title .. "   " .. table.concat(parts, " ")
  end
  state.menu:setTitle(title)
end

setMode = function(index)
  state.index = index
  hs.settings.set(SETTINGS_KEY, currentMode().name)
  drawMarker()
end

local function setKnob(index)
  state.knob = index
  hs.settings.set(KNOB_SETTINGS_KEY, currentKnob().name)
  drawMarker()
end

-- Knob-mode picker: SELECT → knob press → turn → knob press. Mode, or
-- KNOB_SELECT_TIMEOUT_SECONDS of nothing, also leaves it.
local KNOB_SELECT_TIMEOUT_SECONDS = 5

local function knobSelectEnd()
  state.knobSelecting = false
  if state.knobSelectTimer then state.knobSelectTimer:stop(); state.knobSelectTimer = nil end
  drawMarker()
end

local function knobSelectTouch()
  if state.knobSelectTimer then state.knobSelectTimer:stop() end
  state.knobSelectTimer = hs.timer.doAfter(KNOB_SELECT_TIMEOUT_SECONDS, knobSelectEnd)
end

local function knobSelectBegin()
  state.knobSelecting = true
  knobSelectTouch()
  drawMarker()
end

local function knobTurn(direction) -- "ccw" or "cw"
  return function()
    if state.knobSelecting then
      local step = (direction == "cw") and 1 or -1
      setKnob((state.knob - 1 + step) % #KNOB_MODES + 1)
      knobSelectTouch()
      return
    end
    local k = currentKnob()
    if k[direction] then k[direction]() return end
    currentMode()[direction == "ccw" and "up" or "down"]()
  end
end

-- Mode selection. The pad sends modifier combos (Mode, No, knob press) as an
-- instant press+release burst however long it is held, so Mode can't be
-- held: double-tap Mode = SELECT; single tap is reserved.
local DOUBLE_TAP_SECONDS = 0.4
local SELECT_TIMEOUT_SECONDS = 5

local function selectEnd()
  state.selecting = false
  if state.selectTimer then state.selectTimer:stop(); state.selectTimer = nil end
  drawMarker()
end

local function selectBegin()
  state.selecting = true
  state.selectTimer = hs.timer.doAfter(SELECT_TIMEOUT_SECONDS, selectEnd)
  drawMarker()
end

local function modeTap()
  local now = hs.timer.secondsSinceEpoch()
  if state.selecting then
    selectEnd()
  elseif state.knobSelecting then
    knobSelectEnd()
  elseif state.lastModeTap and now - state.lastModeTap < DOUBLE_TAP_SECONDS then
    state.lastModeTap = nil
    selectBegin()
  else
    state.lastModeTap = now -- single tap: reserved
  end
end

-- Called by every button. Returns true if the press was consumed by SELECT.
local function select(button)
  if not state.selecting then return false end
  if button == "knob" then
    selectEnd()
    knobSelectBegin()
    return true
  end
  for _, k in ipairs(SELECT_KEYS) do
    if k.button == button then
      local i = modes.findMode("name", k.mode)
      if i then setMode(i) end
    end
  end
  selectEnd() -- any button leaves SELECT, matched or not
  return true
end

-- Talk: tap = Enter (or the mode's `tap`); hold past TALK_HOLD_SECONDS =
-- push-to-talk until release.
local TALK_HOLD_SECONDS = 0.25

local function talkPress()
  if select(1) then return end
  if state.talkHeld then return end
  if state.dictating then -- a release went missing; this press stops it
    state.talkRecording, state.dictating = false, false
    drawMarker()
    state.recorder.stop()
    return
  end
  state.talkHeld, state.talkRecording = true, false
  state.talkHoldTimer = hs.timer.doAfter(TALK_HOLD_SECONDS, function()
    state.talkHoldTimer = nil
    state.talkRecording, state.dictating = true, true
    drawMarker()
    state.recorder.start(currentMode())
  end)
end

local function talkRelease()
  if not state.talkHeld then return end -- press was used by SELECT
  state.talkHeld = false
  if state.talkHoldTimer then state.talkHoldTimer:stop(); state.talkHoldTimer = nil end
  if state.talkRecording then
    state.talkRecording, state.dictating = false, false
    drawMarker()
    state.recorder.stop()
  else
    (currentMode().tap or modes.tap({}, "return"))()
  end
end

local function role(name)
  return function() currentMode()[name]() end
end

local function unlessSelecting(button, fn)
  return function() if not select(button) then fn() end end
end

local function bind(mods, key, pressed, released)
  table.insert(state.hotkeys, hs.hotkey.new(mods, key, pressed, released))
end

-- Restore the last modes.
state.index = modes.findMode("name", hs.settings.get(SETTINGS_KEY)) or 1
local savedKnob = hs.settings.get(KNOB_SETTINGS_KEY)
for i, k in ipairs(KNOB_MODES) do
  if k.name == savedKnob then state.knob = i end
end

-- Talk (F13) is an eventtap, not a hotkey, so its key events are swallowed
-- before any app or macOS Dictation sees them (a real key event ends
-- Dictation). F13 with modifiers is left alone.
local F13 = hs.keycodes.map.f13
local AUTOREPEAT = hs.eventtap.event.properties.keyboardEventAutorepeat
state.talkTap = hs.eventtap.new({ hs.eventtap.event.types.keyDown, hs.eventtap.event.types.keyUp }, function(ev)
  if ev:getKeyCode() ~= F13 then return false end
  local f = ev:getFlags()
  if f.ctrl or f.alt or f.cmd or f.shift then return false end
  if ev:getType() == hs.eventtap.event.types.keyDown then
    if ev:getProperty(AUTOREPEAT) == 0 then talkPress() end
  else
    talkRelease()
  end
  return true -- swallow
end)
bind({}, "f18", unlessSelecting(2, role("yes")))
bind({ "ctrl", "alt" }, "f16", nil, unlessSelecting(3, role("no")))
bind({}, "f16", unlessSelecting(4, function() (currentMode().clear or modes.clearText)() end))
bind({}, "f17", unlessSelecting(5, role("a")))
bind({ "ctrl", "alt" }, "f19", nil, modeTap)
bind({}, "f19", knobTurn("ccw"))
bind({}, "f20", knobTurn("cw"))
bind({ "ctrl", "alt" }, "f20", nil, function()
  if state.knobSelecting then knobSelectEnd() return end
  if select("knob") then return end
  (currentKnob().press or currentMode().select)()
end)

-- Keys, the Talk eventtap and the menu item are live only while the pad is
-- plugged in, so F13–F20 are free otherwise.
local function setEnabled(on)
  if on == state.enabled then return end
  state.enabled = on
  for _, hk in ipairs(state.hotkeys) do
    if on then hk:enable() else hk:disable() end
  end
  if on then
    state.talkTap:start()
    drawMarker()
    state.menu:returnToMenuBar()
  else
    state.talkTap:stop()
    if state.talkHoldTimer then state.talkHoldTimer:stop(); state.talkHoldTimer = nil end
    state.talkHeld, state.talkRecording, state.dictating = false, false, false
    state.recorder.stop()
    if state.menu then state.menu:removeFromMenuBar() end
  end
end

local function present()
  for _, d in ipairs(hs.usb.attachedDevices() or {}) do
    if d.vendorID == VENDOR and d.productID == PRODUCT then return true end
  end
  return false
end

state.usbWatcher = hs.usb.watcher.new(function(ev)
  if ev.vendorID == VENDOR and ev.productID == PRODUCT then setEnabled(ev.eventType == "added") end
end):start()
drawMarker()
state.menu:removeFromMenuBar()
setEnabled(present())

function state.status()
  return string.format("plugged=%s mode=%s knob=%s", tostring(state.enabled), currentMode().name,
    currentKnob().name)
end

return state
