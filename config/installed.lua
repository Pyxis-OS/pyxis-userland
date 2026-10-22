-- The defaults the installer still publishes to the ESP configuration set,
-- which task 2b retires. Boot no longer reads this file: an installed boot
-- reads system://config/, whose initial copy is config/system/boot.lua.
return {
  hostname = "pyxis",
  input = {
    mouse_scroll_direction = "natural",
    touchpad_scroll_direction = "natural",
    touchpad_speed = 0,
    num_lock = false,
    diagnostics = false,
  },
  volumes = {
    system = { kind = "npfs", partition = 2, volume = "system" },
    home = { kind = "npfs", partition = 2, volume = "home" },
    packages = { kind = "npfs", partition = 2, volume = "packages" },
    ["http-work"] = { kind = "npfs", partition = 2, volume = "http-work" },
  },
  spaces = {
    { name = "pyxis", title = "Pyxis", init = "boot://init-installed",
      network = true, focus = true, launch = true, jobs = true, screenshot = true, power = true,
      clipboard_local = true, clipboard_shared = true,
      package_management = true,
      grants = { http_staging = { volume = "http-work", kind = "directory",
                                 access = "read-write", optional = true },
                 packages = { volume = "packages", kind = "directory",
                              access = "read-only", optional = true } },
      roots = { system = "read-write",
                home = { access = "read-write", optional = true } } },
  },
}
