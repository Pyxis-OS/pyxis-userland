# Wi-Fi controller

`wifi status` observes the declared controller READ grant. `wifi scan` uses the
separate SCAN grant and prints one complete passive result. Neither can associate.

For an explicitly configured open test AP:

```text
link=$(wifi connect --profile home://wifi-test.lua)
wifi status $link
wifi disconnect $link
unset link
```

The finite native profile (at most 4096 bytes) is literal Lua table data:

```lua
return {security="open", ssid="<test-ssid>", bssid="02:00:00:00:00:01", channel=1}
```

Use the actual private BSSID/channel. The tool does not execute profile code,
select another AP or reconnect. CONNECT must be declared separately, and boot
init must have installed the actual country/channel policy before transmission.
An empty typed output capture is required before any association work. Closing
its final owning lease disconnects; a READ-only observation cannot keep it alive.
A configured session's private revocation root also bounds the lease lifetime.

This first station profile is open, legacy 20 MHz on firmware-permitted 2.4 GHz channels; it provides
no cryptographic peer authentication or encryption. `data-ready` is L2 admission,
not an IP address. The trusted per-interface maintainer obtains the independent
`wlan0` address; Ethernet routing, DNS and management choices remain explicit.
