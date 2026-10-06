-- Boot init's spaces for an installed system. system://config/boot.lua on the
-- pool can replace these spaces by name or add more; the rescue boot entry
-- ignores it.
return {
  volumes = {
    system = { kind = "npfs", partition = 2, volume = "system" },
  },
  spaces = {
    { name = "pyxis", title = "Pyxis", init = "boot://init-installed",
      network = true,
      roots = { system = "read-write" } },
  },
}
