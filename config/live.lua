-- Boot init's spaces for live and development boots, which bind no disk.
-- Tab order follows this list, after Caelum's log space.
return {
  volumes = {
    host = { kind = "virtio-fs" },
  },
  spaces = {
    { name = "development", title = "Development", init = "boot://init",
      network = true,
      roots = { host = { access = "read-write", optional = true } } },
    { name = "readonly", title = "Read-only", init = "boot://init-readonly",
      roots = { host = { access = "read-only", optional = true } } },
    { name = "remote", title = "Remote", init = "boot://init-remote",
      roots = { host = { access = "read-write", optional = true } } },
  },
}
