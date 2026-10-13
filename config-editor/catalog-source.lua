-- Trusted build inputs, evaluated only by the host catalog generator. Values
-- come from these existing defaults; this file declares their disposition.
-- Limine paths belong to the parent image configuration. Add Limine entries
-- through an explicit revision catalog rather than guessing them here.
return {
  catalog_id = "pyxis.configuration",
  installed = "config/installed.lua",
  session = "config/session.lua",
  network = "config/network.lua",
  required = false,
  required_entries = {},
}
