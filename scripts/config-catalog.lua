-- Run from the userland repository root:
-- lua scripts/config-catalog.lua REVISION > catalog.lua
-- This host generator evaluates trusted repository defaults only. Its output
-- is bounded declarative data for the native parser, never executable migration.
local function generate()
  local revision = arg[1]
  assert(#arg == 1 and revision and #revision > 0 and #revision <= 63 and
    revision ~= "." and revision ~= ".." and not revision:find("[^!-~]") and
    not revision:find("/", 1, true), "supply one installer source revision component (1..63 bytes)")

  local source = assert(loadfile("config-editor/catalog-source.lua"))()
  local function read(path)
    local value = assert(loadfile(path))()
    assert(type(value) == "table" and not getmetatable(value), "invalid trusted defaults: " .. path)
    return value
  end
  assert(type(source.catalog_id) == "string" and #source.catalog_id > 0 and
    #source.catalog_id <= 4096 and not source.catalog_id:find("\0", 1, true), "invalid source catalog_id")
  assert(type(source.required) == "boolean" and type(source.required_entries) == "table",
    "invalid source declaration disposition")
  local installed, session, network = read(source.installed), read(source.session), read(source.network)
  local catalog = {catalog_id = source.catalog_id, revision = revision, entries = {}}
  local ids, used_required = {}, {}

  local function stable_id(file, kind, name)
    local prefix = file .. "." .. kind .. "."
    if #prefix + #name <= 64 and name:match("^[A-Za-z0-9][A-Za-z0-9_.:%-]*$") then
      return prefix .. name
    end
    -- Non-label/long names retain a deterministic bounded identity. A collision
    -- is refused below; an ID is not authority or a cryptographic identity.
    local hash = 0xcbf29ce484222325
    for i = 1, #name do hash = (hash ~ name:byte(i)) * 0x100000001b3 end
    return prefix .. "hash." .. string.format("%016x", hash)
  end
  local function add(file, kind, name, value)
    assert(type(name) == "string" and #name > 0 and #name <= 4096 and
      not name:find("\0", 1, true), "invalid target name")
    local id = stable_id(file, kind, name)
    assert(not ids[id], "duplicate generated stable ID: " .. id)
    ids[id] = true
    local required = source.required_entries[id]
    if required == nil then required = source.required else used_required[id] = true end
    assert(type(required) == "boolean", "invalid required disposition: " .. id)
    assert(#catalog.entries < 128, "source catalog exceeds 128 entries")
    catalog.entries[#catalog.entries + 1] = {
      id = id, file = file, kind = kind, name = name, required = required, default = value,
    }
  end
  assert(type(installed.volumes) == "table" and type(installed.spaces) == "table", "invalid installed defaults")
  for name, value in pairs(installed.volumes) do
    if name ~= "system" then
      assert(type(value) == "table", "volume default must be a table")
      add("boot", "volume", name, value)
    end
  end
  local space_count = 0
  for key, value in pairs(installed.spaces) do
    assert(math.type(key) == "integer" and key >= 1 and type(value) == "table", "spaces must be a dense array")
    space_count = space_count + 1
    add("boot", "space", value.name, value)
  end
  for i = 1, space_count do assert(installed.spaces[i], "spaces must be a dense array") end
  for name, value in pairs(session) do add("session", "setting", name, value) end
  for name, value in pairs(network) do add("network", "setting", name, value) end
  for id in pairs(source.required_entries) do
    assert(used_required[id], "required override does not name a source entry: " .. tostring(id))
  end
  table.sort(catalog.entries, function(a, b) return a.id < b.id end)

  local fragments, bytes, nodes, active = {}, 0, 0, {}
  local function emit(text)
    bytes = bytes + #text
    assert(bytes <= 1024 * 1024, "source catalog exceeds 1 MiB")
    fragments[#fragments + 1] = text
  end
  local function quote(text)
    assert(#text <= 1024 * 1024, "source string exceeds 1 MiB")
    emit('"')
    local run = 1
    for i = 1, #text do
      local byte = text:byte(i)
      if byte < 32 or byte == 127 or byte == 34 or byte == 92 then
        emit(text:sub(run, i - 1))
        if byte == 34 or byte == 92 then emit("\\" .. string.char(byte))
        else emit(string.format("\\x%02x", byte)) end
        run = i + 1
      end
    end
    emit(text:sub(run))
    emit('"')
  end
  local type_order = {number = 1, string = 2, boolean = 3}
  local function sorted_keys(value)
    local keys = {}
    for key in pairs(value) do
      assert(type_order[type(key)], "unsupported table key in trusted defaults")
      assert(#keys < 8192, "source table exceeds literal value limit")
      keys[#keys + 1] = key
    end
    table.sort(keys, function(a, b)
      if type(a) ~= type(b) then return type_order[type(a)] < type_order[type(b)] end
      if type(a) == "boolean" then return not a and b end
      return a < b
    end)
    return keys
  end
  local function serialize(value, depth)
    nodes = nodes + 1
    assert(nodes <= 8192, "source catalog exceeds 8192 literal values")
    local kind = type(value)
    if kind == "string" then quote(value)
    elseif kind == "boolean" then emit(tostring(value))
    elseif kind == "number" then
      assert(value == value and value ~= math.huge and value ~= -math.huge, "nonfinite source number")
      if math.type(value) == "integer" then emit(string.format("%d", value))
      else
        local number = string.format("%.17g", value)
        if not number:find("[.eE]") then number = number .. ".0" end
        emit(number)
      end
    elseif kind == "table" then
      assert(depth < 32 and not active[value] and not getmetatable(value), "deep/cyclic/metatable source data")
      active[value] = true
      emit("{\n")
      for _, key in ipairs(sorted_keys(value)) do
        emit(string.rep("  ", depth + 1) .. "[")
        serialize(key, depth + 1)
        emit("] = ")
        serialize(value[key], depth + 1)
        emit(",\n")
      end
      emit(string.rep("  ", depth) .. "}")
      active[value] = nil
    else error("unsupported trusted default value: " .. kind) end
  end
  emit("return ")
  serialize(catalog, 0)
  emit("\n")
  assert(io.write(table.concat(fragments)))
end

local ok, reason = pcall(generate)
if not ok then
  io.stderr:write("config-catalog: ", tostring(reason), "\n")
  os.exit(1)
end
