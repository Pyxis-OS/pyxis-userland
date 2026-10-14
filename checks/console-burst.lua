-- console-burst.lua [WRITES] [LINES]: write WRITES runs of LINES empty lines to
-- standard output, flushing after each run, and report the mean time per run.

local function count(index, default, maximum, name)
  local value = arg[index] == nil and default or tonumber(arg[index])
  if not value or value < 1 or value > maximum or value % 1 ~= 0 then
    io.stderr:write("usage: console-burst.lua [WRITES] [LINES]; ", name, " must be an integer from 1 to ", maximum, "\n")
    os.exit(1)
  end
  return value
end

local writes = count(1, 40, 100000, "WRITES")
local lines = count(2, 256, 65536, "LINES")
local now = pyxis.monotonic_ns
local text = string.rep("\n", lines)

local start = now()
for _ = 1, writes do
  io.write(text)
  io.flush()
end
local elapsed = now() - start

print(string.format("console-burst writes=%d lines=%d us_per_write=%d",
  writes, lines, elapsed // writes // 1000))
