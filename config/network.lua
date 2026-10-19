return {
  wired = { auto = true, exclude = {} },
  interfaces = {},
  routing = { auto = true },
  dns = { server = "1.1.1.1" },
  management = { remote = "eth0", log = "eth0", debug = "eth0" },
}
