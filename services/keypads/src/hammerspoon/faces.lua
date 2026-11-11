-- keypads: render an emoji key face and pack it for the LilyGO.
-- Design: docs/architecture/keypads.md ("Key faces").
--
-- A face is 128×128 RGB565: an emoji, or a short word in the system font
-- (sized to fit), centred on a solid background.
-- Packed format (unpacked by proto::unpackFace on the device): packets of a
-- header byte h, then
--   h < 128:  h + 1 literal pixels (2 bytes each, little-endian)
--   h >= 128: one pixel repeated h - 126 times (2..129)

local M = {}

local SIZE = 128
local EMOJI_POINTS = 88
local TEXT_MAX_POINTS = 40
local TEXT_MAX_WIDTH = 108 -- leaves a margin: panel edges sit under the keycap rim
local FORMAT_VERSION = "1" -- bump to re-render every face (e.g. layout change)

local function rgb(hex)
  return {
    red = tonumber(hex:sub(1, 2), 16) / 255,
    green = tonumber(hex:sub(3, 4), 16) / 255,
    blue = tonumber(hex:sub(5, 6), 16) / 255,
  }
end

-- Content-addressed name, safe as a device file name ([a-z0-9], 12 chars).
function M.name(content, bgHex, isText)
  local kind = isText and "t" or "e"
  return hs.hash.SHA1(FORMAT_VERSION .. "|" .. kind .. "|" .. content .. "|" .. bgHex):sub(1, 12):lower()
end

-- Bold system-font text, shrunk until it fits TEXT_MAX_WIDTH.
local function fittedText(text)
  local size = TEXT_MAX_POINTS
  while true do
    local styled = hs.styledtext.new(text, {
      font = { name = ".AppleSystemUIFontBold", size = size },
      color = { white = 1 },
      paragraphStyle = { alignment = "center" },
    })
    local dims = hs.drawing.getTextDrawingSize(styled)
    if dims.w <= TEXT_MAX_WIDTH or size <= 10 then return styled, dims.h end
    size = size - 2
  end
end

local function to565(c)
  local r = math.floor((c.red or 0) * 31 + 0.5)
  local g = math.floor((c.green or 0) * 63 + 0.5)
  local b = math.floor((c.blue or 0) * 31 + 0.5)
  return (r << 11) | (g << 5) | b
end

local function pack(px)
  local out, n, i = {}, #px, 1
  while i <= n do
    local run = 1
    while i + run <= n and px[i + run] == px[i] and run < 129 do run = run + 1 end
    if run >= 2 then
      out[#out + 1] = string.pack("<BI2", run + 126, px[i])
      i = i + run
    else
      local j = i
      while j <= n and j - i < 128 and not (j + 1 <= n and px[j + 1] == px[j]) do j = j + 1 end
      if j == i then j = i + 1 end -- always at least one literal
      local count = j - i
      local parts = { string.pack("B", count - 1) }
      for k = i, j - 1 do parts[#parts + 1] = string.pack("<I2", px[k]) end
      out[#out + 1] = table.concat(parts)
      i = j
    end
  end
  return table.concat(out)
end

-- Renders and packs a face. Takes ~250 ms (pixel reads), so callers cache
-- by name and the device keeps faces in flash.
function M.render(content, bgHex, isText)
  local c = hs.canvas.new({ x = 0, y = 0, w = SIZE, h = SIZE })
  c[1] = { type = "rectangle", action = "fill", fillColor = rgb(bgHex),
           frame = { x = 0, y = 0, w = SIZE, h = SIZE } }
  if isText then
    local styled, h = fittedText(content)
    c[2] = { type = "text", frame = { x = 0, y = (SIZE - h) / 2, w = SIZE, h = h + 4 }, text = styled }
  else
    c[2] = { type = "text", frame = { x = 0, y = (SIZE - EMOJI_POINTS * 1.2) / 2, w = SIZE, h = SIZE },
             text = hs.styledtext.new(content, { font = { size = EMOJI_POINTS },
                                                 paragraphStyle = { alignment = "center" } }) }
  end
  local img = c:imageFromCanvas()
  c:delete()
  local px = {}
  for y = 0, SIZE - 1 do
    for x = 0, SIZE - 1 do px[#px + 1] = to565(img:colorAt({ x = x, y = y })) end
  end
  return pack(px)
end

return M
