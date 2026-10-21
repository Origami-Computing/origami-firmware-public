-- Copy to /widgets/counter/main.lua on the SD card, then open Lua Widget.
-- State lasts for this widget session; this example does not write storage.
local count = 0
local bg = kami.draw.rgb(12, 14, 20)
local fg = kami.draw.rgb(240, 244, 250)
local accent = kami.draw.rgb(120, 200, 255)
local muted = kami.draw.rgb(160, 170, 185)

function widget.start()
  count = 0
end

function widget.input(event)
  if event.type == "scroll" then
    count = math.max(0, math.min(999, count + event.delta))
  elseif event.type == "A" then
    count = 0
  end
end

function widget.draw(now)
  local w, h = kami.W, kami.H
  kami.draw.rect("bg", 0, 0, w, h, bg)
  kami.draw.label("title", 0, 16, w, 26, "Little Counter", fg, bg, 2, 1)
  kami.draw.label("count", 0, 62, w, 64, tostring(count), accent, bg, 4, 2)
  kami.draw.progress("progress", 24, h - 66, w - 48, 10, count, 999,
                     kami.draw.rgb(36, 42, 52), accent)
  kami.draw.label("hint", 0, h - 40, w, 16, "Turn: count  Select: reset", muted, bg, 1, 1)
  kami.draw.label("back", 0, h - 22, w, 16, "LB: back", muted, bg, 1, 1)
end
