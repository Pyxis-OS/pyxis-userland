-- Boot init's spaces for live and development boots, which bind no disk.
-- Tab order follows this list, after Caelum's log space. home:// is RAM and
-- lasts until reboot; spaces start in it unless start names another root.
return {
  hostname = "pyxis",
  input = {
    mouse_scroll_direction = "natural",
    touchpad_scroll_direction = "natural",
    diagnostics = false,
  },
  volumes = {
    host = { kind = "virtio-fs" },
    home = { kind = "ram" },
    ["http-work"] = { kind = "ram" },
  },
  spaces = {
    { name = "development", title = "Development", init = "boot://init",
      network = true, launch = true, jobs = true, screenshot = true, power = true,
      clipboard_local = true, clipboard_shared = true,
      grants = { http_staging = { volume = "http-work", kind = "directory",
                                 access = "read-write", optional = true } },
      roots = { host = { access = "read-write", optional = true }, home = "read-write" } },
    -- init-readonly skips network setup; Development owns it, while these roots stay writable.
    { name = "multiplexer", title = "Multiplexer", init = "boot://init-readonly",
      launch = true, jobs = true, screenshot = true, power = true, multiplexer = true,
      clipboard_local = true, clipboard_shared = true,
      grants = { http_staging = { volume = "http-work", kind = "directory",
                                 access = "read-write", optional = true } },
      roots = { host = { access = "read-write", optional = true }, home = "read-write" } },
    { name = "readonly", title = "Read-only", init = "boot://init-readonly",
      clipboard_local = true, clipboard_shared = true,
      grants = { http_staging = { volume = "http-work", kind = "directory",
                                 access = "read-write", optional = true } },
      roots = { host = { access = "read-only", optional = true }, home = "read-only" } },
    { name = "remote", title = "Remote", init = "boot://init-remote",
      launch = true, remote_multiplexer = true, screenshot = true, remote_power = true,
      grants = { http_staging = { volume = "http-work", kind = "directory",
                                 access = "read-write", optional = true } },
      roots = { host = { access = "read-write", optional = true }, home = "read-write" },
      start = "tmp" },
  },
}
