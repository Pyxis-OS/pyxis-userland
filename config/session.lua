return {
  timezone = "Europe/Bucharest",
  terminal = { tab_width = 8 },
  retention = {
    value_limit_bytes = 67108864,
    session_limit_bytes = 536870912,
  },
  environment = {
    -- The terminal profile: exactly the sequences in Pyxis docs/userland/terminal.md.
    TERM = "pyxis",
    -- The vi editor's EXINIT: three-column tab stops, spaces for Tab, autoindent.
    EXINIT = "set ts=3 et ai",
  },
}
