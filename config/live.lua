-- Boot init's spaces for live and development boots, which bind no disk.
-- Tab order follows this list, after Caelum's log space. home:// is RAM and
-- lasts until reboot; spaces start in it unless start names another root.
return {
  hostname = "pyxis",
  volumes = {
    host = { kind = "virtio-fs" },
    home = { kind = "ram" },
  },
  spaces = {
    { name = "development", title = "Development", init = "boot://init",
      network = true, launch = true, screenshot = true, power = true,
      clipboard_local = true, clipboard_shared = true,
      roots = { host = { access = "read-write", optional = true }, home = "read-write" } },
    { name = "readonly", title = "Read-only", init = "boot://init-readonly",
      clipboard_local = true, clipboard_shared = true,
      roots = { host = { access = "read-only", optional = true }, home = "read-only" } },
    { name = "remote", title = "Remote", init = "boot://init-remote",
      screenshot = true, remote_power = true,
      roots = { host = { access = "read-write", optional = true }, home = "read-write" },
      start = "tmp" },
  },
}
