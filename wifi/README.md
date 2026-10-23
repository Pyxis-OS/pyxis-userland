# Wi-Fi controller

`wifi status` observes the declared controller READ grant. `wifi scan` uses the
separate SCAN grant and prints one complete passive result. Neither can associate.

Status appends `boot valid=... state=...` hexadecimal masks for known boot facts
and completed actions. Only valid groups print reset, cut, security, firmware or
board lines; unknown groups remain absent. Board country bytes print as four
hexadecimal digits, not text or a regulatory permission. Firmware readiness and
stop observations do not grant scan/data readiness or prove RF silence.

For an explicitly configured 2.4 GHz AP:

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
select another AP. Set `security="wpa2-psk"` for WPA2-Personal/CCMP; the tool
prompts through terminal input without echo or history, never an argument or a
profile key. It requires console stdin/stderr, separately from the typed stdout.
WPA2 may reconnect to the same AP after an ordinary drop while the lease and
configured session remain live. Explicit disconnect/CUT stops recovery. CONNECT must be declared separately, and boot
init must have installed the actual country/channel policy before transmission.
An empty typed output capture is required before any association work. Closing
its final owning lease disconnects; a READ-only observation cannot keep it alive.
A configured session's private revocation root also bounds the lease lifetime.

Both profiles remain legacy 20 MHz on firmware-permitted 2.4 GHz channels.
`open` supplies no peer authentication or encryption. WPA2 requires the trusted
session supplicant and a complete handshake with confirmed pairwise/group keys
before any ordinary data is admitted. Optional PMF is not negotiated; required
PMF/SAE-only APs are refused. A WPA2+WPA3 transition AP may select WPA2. `data-ready` is L2 admission,
not an IP address. The trusted per-interface maintainer obtains the independent
`wlan0` address; Ethernet routing, DNS and management choices remain explicit.

On RTL8922AE, status also reports pinned regdb rules and independent platform
input states. Rule EIRP is in mBm; flags preserve NO_IR/DFS/environment restrictions.
Omitted country selects world `00`; a configured country selects its rules within
the host mask. Table `valid` means decoded metadata, and `default-power=1`
identifies the accepted reference RFE-0 selection. Neither that state nor a
matching rule means RFK is qualified or that RF transmission is admitted.
Missing, malformed and failed platform reads remain distinct. The input observer
adds no RF commands; RTL scan/connect remain unavailable in this cut.

TAS stays disabled. Its diagnostic identifies query-0, function-5 or alias
rejection, with the observed object type and any known length/support mask.
The fixed SAR observation normalizes every RWRD profile, intersecting present
WRDS independently. It reports the source CID/revision/shape and separate path
A/B 1TX/2TX ceilings in eighth-dBm, including the source-specific 2TX reduction.
RWSI cannot select a less restrictive profile; TAS cannot boost the result.
Unknown or bad required sources yield no numeric limits. These are SAR inputs,
not a claim that regulatory, board and antenna limits or calibration are applied.

Trusted `wifi_policy` configuration may omit country (or use `"00"`) for RTL
world-domain rules while supplying the narrowing channel mask. Country `"00"`
names the same world domain in the shared policy contract; AX200 Install support
is being integrated in [#1159](https://git.internal/PyxisOS/pyxis-os/pulls/1159),
with its authoritative firmware LAR restrictions retained.
