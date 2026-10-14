-- cpu-busy.lua [SECONDS]: spin on the monotonic clock for SECONDS and report
-- how many iterations completed, a measure of this task's progress under load.
-- Start several in the background (cpu-busy.lua &) to occupy more CPUs.

local function count(index, default, maximum, name)
  local value = arg[index] == nil and default or tonumber(arg[index])
  if not value or value < 1 or value > maximum or value % 1 ~= 0 then
    io.stderr:write("usage: cpu-busy.lua [SECONDS]; ", name, " must be an integer from 1 to ", maximum, "\n")
    os.exit(1)
  end
  return value
end

local seconds = count(1, 24, 3600, "SECONDS")
local now = pyxis.monotonic_ns
local deadline = now() + seconds * 1000000000

local iterations = 0
while now() < deadline do
  iterations = iterations + 1
end

print(string.format("cpu-busy seconds=%d iterations=%d", seconds, iterations))
