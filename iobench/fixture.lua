-- Byte i is (i XOR (i >> 8) XOR (i >> 16) XOR 0xa5) modulo 256.
-- Keep this definition in step with the verifier in common.c.
local size = assert(math.tointeger(tonumber(arg[1] or "1048576")))
assert(size > 0 and size <= 1048576)
for base = 0, size - 1, 4096 do
  local bytes = {}
  for i = base, math.min(base + 4095, size - 1) do
    bytes[#bytes + 1] = string.char((i ~ (i >> 8) ~ (i >> 16) ~ 0xa5) & 0xff)
  end
  assert(io.write(table.concat(bytes)))
end
assert(io.flush())
