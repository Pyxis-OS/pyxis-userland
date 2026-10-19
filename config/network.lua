return {
  wired = { auto = true, exclude = {} },
  interfaces = {},
  routing = { default = "eth0" },
  dns = { interface = "eth0", server = "1.1.1.1" },
  management = { remote = "eth0", log = "eth0", debug = "eth0" },
}
