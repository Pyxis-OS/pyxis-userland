-- An installed system's spaces, volumes and machine defaults. Install copies
-- this file, session.lua, network.lua and init-installed from
-- boot://share/installer/system/ to system://config/ once; after that they
-- belong to the administrator and boot reads only them (docs/userland/init.md).
-- Volumes without a partition are on the pool that mount.pool names. home is
-- optional so that a damaged home volume still starts pyxis, in tmp://.
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
    system = { kind = "npfs", volume = "system" },
    home = { kind = "npfs", volume = "home" },
    packages = { kind = "npfs", volume = "packages" },
    ["http-work"] = { kind = "npfs", volume = "http-work" },
  },
  spaces = {
    { name = "pyxis", title = "Pyxis", init = "system://config/init-installed",
      network = true, focus = true, launch = true, jobs = true, screenshot = true, power = true,
      clipboard_local = true, clipboard_shared = true,
      package_management = true,
      grants = { http_staging = { volume = "http-work", kind = "directory",
                                 access = "read-write", optional = true },
                 packages = { volume = "packages", kind = "directory",
                              access = "read-only", optional = true } },
      roots = { system = "read-write",
                home = { access = "read-write", optional = true },
                pkg = { view = "pkg", optional = true } } },
  },
}
