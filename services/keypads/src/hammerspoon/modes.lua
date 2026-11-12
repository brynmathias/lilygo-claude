-- keypads: mode definitions and actions shared by every pad.
-- Moved from min-claude-keyboard's init.lua; behaviour is unchanged.
-- Design: docs/architecture/keypads.md

local M = {}

local function tap(mods, key)
  return function() hs.eventtap.keyStroke(mods, key, 0) end
end
M.tap = tap

-- Media/system keys (volume, play, brightness), with the on-screen HUD.
local function systemKey(name)
  return function()
    hs.eventtap.event.newSystemKeyEvent(name, true):post()
    hs.eventtap.event.newSystemKeyEvent(name, false):post()
  end
end

local function scroll(lines)
  return function() hs.eventtap.scrollWheel({ 0, lines }, {}, "line") end
end

-- Clear typed text. Depends on the app in front, not the mode.
-- In a terminal (Claude Code) kill line by line: Claude Code has no single
-- clear-input key (Ctrl+L is a redraw). Per line: Ctrl+K kills to end,
-- Ctrl+U to start, Backspace joins the previous line. Verified 2.1.318.
local CLEAR_MAX_LINES = 12
local TERMINALS = {
  ["iTerm2"] = true, ["Terminal"] = true, ["Ghostty"] = true,
  ["WezTerm"] = true, ["Warp"] = true, ["kitty"] = true, ["Alacritty"] = true,
}

function M.clearText()
  local app = hs.application.frontmostApplication()
  if app and TERMINALS[app:name()] then
    for _ = 1, CLEAR_MAX_LINES do
      hs.eventtap.keyStroke({ "ctrl" }, "k", 0)
      hs.eventtap.keyStroke({ "ctrl" }, "u", 0)
      hs.eventtap.keyStroke({}, "delete", 0)
    end
  else
    hs.eventtap.keyStroke({ "cmd" }, "a", 0)
    hs.eventtap.keyStroke({}, "delete", 0)
  end
end

-- Modes, in menu order. Roles every mode has: up/down/select (knob),
-- yes/no/a (mode-specific buttons). Optional: talk ("claude" = Claude
-- Code voice, default macOS Dictation), tap (what a short Talk tap does,
-- default Enter), clear (default clear typed text).
--
-- `short` and `labels` are ASCII-only names for keys with screens (the
-- fallback if an emoji face can't be sent). `icon` is the mode's emoji;
-- `icons` gives each role's emoji (missing roles use M.ROLE_ICONS).
-- `pad4` maps the LilyGO's keys 2–4 to roles (main unit alone: no Mode
-- key, modes come from the menu bar); `pad9` maps keys 2–7 when the
-- expansion is attached. Talk (1), knob mode (8) and Mode (9) are the same
-- in every mode.
M.MODES = {
  {
    name = "claude", label = "🤖 CLAUDE", short = "Claude", color = { red = 0.85, green = 0.47, blue = 0.34 },
    talk = "claude",
    up = tap({}, "up"), down = tap({}, "down"), select = tap({}, "return"),
    yes = tap({}, "return"), no = tap({}, "escape"),
    a = tap({ "cmd" }, "]"), -- next split pane in this iTerm tab (wraps round)
    -- Hop between Claude sessions in iTerm tabs without leaving the mode.
    tabprev = tap({ "cmd", "shift" }, "["), tabnext = tap({ "cmd", "shift" }, "]"),
    keys = { [2] = "Enter", [3] = "Esc", [5] = "Pane" },
    labels = { yes = "Enter", no = "Stop", a = "Pane", tabprev = "< Tab", tabnext = "Tab >" },
    icon = "🤖", icons = { yes = "✅", no = "🛑", a = "🪟", tabprev = "⬅️", tabnext = "➡️" },
    -- Voice, Tab <, Tab >, Enter, Pane, Clear, Stop, knob mode, Mode.
    pad4 = { [2] = "tabprev", [3] = "tabnext", [4] = "yes" },
    pad9 = { [2] = "tabprev", [3] = "tabnext", [4] = "yes", [5] = "a", [6] = "clear", [7] = "no" },
  },
  {
    name = "iterm", label = "🖥️ ITERM", short = "iTerm", color = { red = 0.45, green = 0.45, blue = 0.5 },
    up = scroll(3), down = scroll(-3), select = tap({}, "return"),
    yes = tap({ "cmd", "shift" }, "["), -- previous iTerm tab
    no = tap({ "cmd", "shift" }, "]"),  -- next iTerm tab
    a = tap({ "cmd" }, "]"),            -- next split pane
    keys = { [2] = "◀Tab", [3] = "Tab▶", [5] = "Pane" },
    labels = { yes = "< Tab", no = "Tab >", a = "Pane" },
    icon = "🖥️", icons = { yes = "⬅️", no = "➡️", a = "🪟" },
    pad4 = { [2] = "clear", [3] = "a" }, pad9 = { [2] = "clear", [3] = "a", [4] = "yes", [5] = "no" },
  },
  {
    name = "browser", label = "🌐 BROWSER", short = "Browser", color = { red = 0.25, green = 0.5, blue = 0.9 },
    up = scroll(3), down = scroll(-3), select = tap({}, "return"),
    yes = tap({ "ctrl", "shift" }, "tab"), -- previous tab
    no = tap({ "ctrl" }, "tab"),           -- next tab
    a = tap({ "cmd" }, "["),               -- back
    clear = tap({ "cmd" }, "w"),           -- close tab instead of clear text
    keys = { [2] = "◀Tab", [3] = "Tab▶", [4] = "Close", [5] = "Back" },
    labels = { yes = "< Tab", no = "Tab >", a = "Back", clear = "Close Tab" },
    icon = "🌐", icons = { yes = "◀️", no = "▶️", a = "🔙", clear = "❌" },
    pad4 = { [2] = "clear", [3] = "a" }, pad9 = { [2] = "clear", [3] = "a", [4] = "yes", [5] = "no" },
  },
  {
    name = "meet", label = "📹 MEET", short = "Meet", color = { red = 0.1, green = 0.6, blue = 0.3 },
    -- Google Meet; the Meet tab must be in front. A short Talk tap toggles
    -- the mic (latched); holding Talk still dictates.
    up = scroll(3), down = scroll(-3), select = tap({}, "return"),
    tap = tap({ "cmd" }, "d"),          -- toggle microphone
    yes = tap({ "ctrl", "cmd" }, "h"),  -- raise / lower hand
    no = tap({ "cmd" }, "e"),           -- toggle camera
    a = tap({ "ctrl", "cmd" }, "c"),    -- captions on / off
    keys = { [1] = "Mic", [2] = "Hand", [3] = "Cam", [5] = "Captions" },
    labels = { talk = "Mic / Talk", yes = "Hand", no = "Camera", a = "Captions" },
    icon = "📹", icons = { talk = "🎤", yes = "✋", no = "📷", a = "💬" },
    pad4 = { [2] = "clear", [3] = "no" }, pad9 = { [2] = "clear", [3] = "no", [4] = "yes", [5] = "a" },
  },
  {
    name = "apps", label = "🧪 APPS", short = "Apps", color = { red = 0.3, green = 0.7, blue = 0.4 },
    up = tap({}, "up"), down = tap({}, "down"), select = tap({}, "return"),
    yes = tap({}, "return"), no = tap({}, "escape"),
    a = tap({}, "tab"),
    keys = { [2] = "Enter", [3] = "Esc", [5] = "Tab" },
    labels = { yes = "Enter", no = "Esc", a = "Tab" },
    icon = "🧪", icons = { yes = "✅", no = "✋", a = "➡️" },
    pad4 = { [2] = "clear", [3] = "no" }, pad9 = { [2] = "clear", [3] = "no", [4] = "yes", [5] = "a" },
  },
}

-- Emoji for roles every mode shares. Key faces are emoji only.
M.ROLE_ICONS = { talk = "🎤", clear = "🧹" }

-- The knob's own mode. "nav" = the button mode's up/down; the others are
-- the keyboard's media keys. Clockwise = up / louder / brighter.
M.KNOB_MODES = {
  { name = "nav", icon = "⇅", text = "Scroll" },
  { name = "volume", icon = "🔊", text = "Volume", ccw = systemKey("SOUND_DOWN"), cw = systemKey("SOUND_UP"),
    press = systemKey("PLAY") },
  { name = "brightness", icon = "☀️", text = "Brightness", ccw = systemKey("BRIGHTNESS_DOWN"), cw = systemKey("BRIGHTNESS_UP") },
}

function M.findMode(field, value)
  for i, m in ipairs(M.MODES) do
    if m[field] == value then return i, m end
  end
end

-- Push-to-talk. One recorder per pad, so releasing Talk on one pad never
-- stops a recording started on the other.
local TALK_REPEAT_SECONDS = 0.03 -- Claude Code detects "hold" via key repeat
-- System Settings > Keyboard > Dictation > Shortcut > Customise… must match.
local DICTATION_SHORTCUT = { { "ctrl", "alt", "cmd" }, "d" }

local function postSpace(isDown, isRepeat)
  local ev = hs.eventtap.event.newKeyEvent({}, "space", isDown)
  if isRepeat then ev:setProperty(hs.eventtap.event.properties.keyboardEventAutorepeat, 1) end
  ev:post()
end

local function dictationToggle()
  hs.eventtap.keyStroke(DICTATION_SHORTCUT[1], DICTATION_SHORTCUT[2], 0)
end

function M.newRecorder()
  local r = { backend = nil, timer = nil }
  -- The backend is chosen at start and remembered, so a mode change
  -- mid-hold still stops the right thing.
  function r.start(mode)
    r.backend = mode.talk or "dictation"
    if r.backend == "dictation" then
      dictationToggle()
    else
      postSpace(true, false)
      r.timer = hs.timer.doEvery(TALK_REPEAT_SECONDS, function() postSpace(true, true) end)
    end
  end
  function r.stop()
    if not r.backend then return end
    if r.backend == "dictation" then
      dictationToggle()
    else
      if r.timer then r.timer:stop(); r.timer = nil end
      postSpace(false, false)
    end
    r.backend = nil
  end
  function r.active() return r.backend ~= nil end
  return r
end

return M
