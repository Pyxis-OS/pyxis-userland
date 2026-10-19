-- Boot init's spaces for an installed system. system://config/boot.lua on the
-- pool can replace these spaces by name or add more; the rescue boot entry
-- ignores it. Spaces start in home:// unless start names another root. home is
-- optional so that a damaged home volume still starts pyxis, in tmp://. The
-- packages source is optional too: programs resolve declared packages through
-- it, and systems installed before packages have no such volume until Update.
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
                home = { access = "read-write", optional = true },
                toolchain = { package = "clang-hosted", optional = true } } },
  },
}
