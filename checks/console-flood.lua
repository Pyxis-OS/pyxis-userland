-- console-flood.lua [SECONDS]: print numbered lines to standard output for
-- SECONDS, flushing after each, and report how many were printed.

local function count(index, default, maximum, name)
  local value = arg[index] == nil and default or tonumber(arg[index])
  if not value or value < 1 or value > maximum or value % 1 ~= 0 then
    io.stderr:write("usage: console-flood.lua [SECONDS]; ", name, " must be an integer from 1 to ", maximum, "\n")
    os.exit(1)
  end
  return value
end

local seconds = count(1, 8, 3600, "SECONDS")
local now = pyxis.monotonic_ns
local deadline = now() + seconds * 1000000000

local lines = 0
repeat
  lines = lines + 1
  io.write(lines, "\n")
  io.flush()
until now() > deadline

print(string.format("console-flood seconds=%d lines=%d lines_per_s=%d",
  seconds, lines, lines // seconds))
