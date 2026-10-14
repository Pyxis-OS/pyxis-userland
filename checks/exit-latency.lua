-- exit-latency.lua [LAUNCHES] [SECONDS]: launch bin://echo.pxe -n until LAUNCHES
-- have exited or SECONDS have passed, timing each from launch through exit, and
-- report the median, 90th percentile and maximum.

local function count(index, default, maximum, name)
  local value = arg[index] == nil and default or tonumber(arg[index])
  if not value or value < 1 or value > maximum or value % 1 ~= 0 then
    io.stderr:write("usage: exit-latency.lua [LAUNCHES] [SECONDS]; ", name, " must be an integer from 1 to ", maximum, "\n")
    os.exit(1)
  end
  return value
end

local launches = count(1, 100, 100000, "LAUNCHES")
local seconds = count(2, 20, 3600, "SECONDS")
local now, run = pyxis.monotonic_ns, pyxis.run
local command = {"bin://echo.pxe", "-n"}

local deadline = now() + seconds * 1000000000
local samples = {}
repeat
  local start = now()
  assert(run(command) == 0, "bin://echo.pxe failed")
  samples[#samples + 1] = (now() - start) // 1000
until #samples == launches or now() > deadline

table.sort(samples)
local n = #samples
print(string.format("exit-latency launches=%d median_us=%d p90_us=%d max_us=%d",
  n, samples[(n + 1) // 2], samples[(n * 9 + 9) // 10], samples[n]))
