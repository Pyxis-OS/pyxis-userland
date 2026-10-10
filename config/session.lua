return {
  timezone = "Europe/Bucharest",
  terminal = { tab_width = 8 },
  environment = {
    -- The terminal profile: exactly the sequences in Pyxis docs/userland/terminal.md.
    TERM = "pyxis",
    -- vi: three-column tab stops, spaces for Tab and autoindent.
    EXINIT = "set ts=3 et ai",
  },
}
