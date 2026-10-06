-- Boot init's spaces for an installed system. system://config/boot.lua on the
-- pool can replace these spaces by name or add more; the rescue boot entry
-- ignores it. Spaces start in home:// unless start names another root. home is
-- optional so that a damaged home volume still starts pyxis, in tmp://.
return {
  volumes = {
    system = { kind = "npfs", partition = 2, volume = "system" },
    home = { kind = "npfs", partition = 2, volume = "home" },
  },
  spaces = {
    { name = "pyxis", title = "Pyxis", init = "boot://init-installed",
      network = true,
      roots = { system = "read-write",
                home = { access = "read-write", optional = true } } },
  },
}
