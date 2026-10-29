# Wi-Fi controller

`wifi status` observes the declared controller READ grant. `wifi scan` uses the
separate SCAN grant and prints a completed passive result. Capacity-limited
results retain what fits and visibly report TRUNCATED with the dropped-candidate
count (a saturated count is a lower bound). Neither command can associate.

Use `wifi scan --channel 1` to request one channel. The argument is a decimal
number from 1 to 14; controller support and installed policy still restrict the
request. `wifi scan --channels 1,6` requests the explicit two-channel sweep.
Plain `wifi scan` retains the controller's default selection; the current
RTL8922AE profile requires an explicit channel or the 1+6 mask.

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
Missing, malformed and failed platform reads remain distinct. RTL8922AE uses
two initialized PHY contexts, returning an immutable FILE only after firmware
scan completion and RX retirement. Default scans select the admitted channel
1–13 set; explicit masks are exact. The firmware period is 232 TU per channel,
within the existing 15-second operation deadline. World channels 12–13 use
listen-only firmware entries; calibration stays on the lowest requested channel
that permits initiation. A first NO_IR-only request refuses before startup,
without borrowing an unrequested channel. The common image intersects firmware,
SAR and numeric regdb limits across the whole mask before calibration.
Frame channels come from matched PPDU metadata; unattributable observations are
omitted. Results keep what fits and report TRUNCATED when capacity is exhausted. Healthy
context is retained for another exact-mask request; changing the mask requires
a fresh boot in this slice. RTL association remains unavailable. Status/READ
alone never starts the radio.

RTL PHY status records the historical preparation phase, state mask, selected
channel/region, power-image readback count and path SAR bounds in quarter-dBm.
It prints attempted rfkill-check counts and observation validity, completed RFK
steps, any valid report state/version and the thermal-default path mask. Stop is reported as confirmed,
uncertain or unobserved; last status is meaningful only after completion.
These are historical programming observations, not measured radiated power.
Startup status separately reports the initial WLMAC state observation and any
inherited-active power cycle. `completed` requires successful power-off and
fresh power-on; firmware readiness or later terminal shutdown is not that proof.
`non-active` is the sampled state, not a physical RF-off claim. Facts remain
available after failure, and `unobserved` remains distinct from not needing a
cycle. The released status prefix and full reply sizes stay unchanged; the
matching SDK reader validates the new state flags together.
A hopping sweep also verifies the common power image after RX collection closes;
that endpoint check is not a measurement of uninterrupted enforcement.

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
names the same world domain in the shared policy contract; AX200 retains its
authoritative firmware LAR restrictions.

`wifi_status()` retains the released 544-byte status prefix. The preparation
command uses `wifi_status_full()` for the complete 600-byte base/PHY snapshot;
partial PHY facts are never published as valid. Neither query starts the radio.
