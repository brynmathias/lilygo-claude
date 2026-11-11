-- keypads loader. Load from ~/.hammerspoon/init.lua with:
--   dofile(os.getenv("HOME") .. "/git/lilygo-claude/services/keypads/src/hammerspoon/init.lua")
-- Design: docs/architecture/keypads.md
--
-- Loads every pad; each one is only active while it is plugged in:
--   lilygo.lua  LilyGO T-Keyboard-S3-Pro (USB serial)
--   ch57x.lua   CH57x 6-key pad (F13–F20, flashed with ../ch57x/keyboard.yaml)
--
-- Hammerspoon console helpers:
--   keypads.lilygo.status()   what the LilyGO link is doing
--   keypads.lilygo.pause()    release the serial port (before flashing)
--   keypads.lilygo.resume()   take it back
--   keypads.ch57x.status()    whether the 6-key pad is plugged in, and its mode

require("hs.ipc") -- lets `hs -c` talk to this config from a terminal

local DIR = debug.getinfo(1, "S").source:match("^@(.*)/[^/]*$")
local modes = dofile(DIR .. "/modes.lua")

keypads = { -- global, so nothing is garbage-collected
  modes = modes,
  faces = dofile(DIR .. "/faces.lua"),
  lilygo = nil,
}
keypads.lilygo = assert(loadfile(DIR .. "/lilygo.lua"))(modes, keypads.faces)

keypads.ch57x = assert(loadfile(DIR .. "/ch57x.lua"))(modes)

-- A reload drops this Lua state but not open serial ports, which then stay
-- busy until Hammerspoon quits. Release the port before every reload/quit.
local previousShutdown = hs.shutdownCallback
hs.shutdownCallback = function()
  keypads.lilygo.pause()
  if previousShutdown then previousShutdown() end
end

return keypads
