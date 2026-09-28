-- Byte i is (i XOR (i >> 8) XOR (i >> 16) XOR 0xa5) modulo 256.
-- Keep this definition in step with the verifier in main.c.
for base = 0, 1024 * 1024 - 1, 4096 do
  local bytes = {}
  for i = base, base + 4095 do
    bytes[#bytes + 1] = string.char((i ~ (i >> 8) ~ (i >> 16) ~ 0xa5) & 0xff)
  end
  assert(io.write(table.concat(bytes)))
end
assert(io.flush())
