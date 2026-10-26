# gamepadtest

A native graphical DualSense Edge USB input viewer. Install the `fonts` package,
then launch `gamepadtest` from a local shell with display, keyboard and gamepad
authority. The bundle declares `gamepad` with `input` rights and a read-only
`fonts` package root. Launch the bundle, not its inner executable.

Escape exits. Tab or Left/Right selects another attached controller. The display
fits a 960 by 640 canvas to the current display size and follows display resize.
The front view shows both sticks, independent analog trigger bars, the hat and
all nineteen digital buttons. Fn and rear paddles are shown below the sticks.
Axis values remain raw unsigned bytes, 0 through 255; hat directions run clockwise
from up (0), with 8 neutral. No deadzone or trigger-button synthesis is applied.

The viewer can acquire an empty catalog and updates when a supported controller
is attached or removed. A replug has a new attachment instance. Selection is
preserved while its instance remains present, otherwise it selects the first
available instance. Catalog changes during info/state collection cause a retry.
Focus changes and reset events clear held input and recover accepted state through
the catalog/state interface. Invalid states show no stick puck or trigger value;
held buttons and hat directions require release/neutral and a fresh action after
a reset. A fresh focused report restores analog values.

The library wrappers in [gamepad.h](../include/gamepad.h) use the SDK's public ABI.
The session, shell and multiplexer forward the ordinary gamepad grant along the
same local input path as keyboard. Bundles still need an explicit declaration;
background, redirected-input and capture jobs gain no device input implicitly.

Build with the selected SDK:

```sh
make -j16 SDK=/path/to/sdk gamepadtest
```
