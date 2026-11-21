# Set up CoffeeCNC on your machine

This guide puts CoffeeCNC on a LinuxCNC machine that already runs with another display, for example AXIS or Gmoccapy. Keep a copy of your configuration folder before you start.

## Requirements

- Debian 13 (trixie), amd64 or arm64.
- LinuxCNC 2.9 (`linuxcnc-uspace`) from the linuxcnc.org `trixie 2.9-uspace` repository. The package works with the 2.9 version it was built against and later 2.9 versions.
- A milling machine with X, Y and Z axes and `trivkins` (joint 0 is X, joint 1 is Y, joint 2 is Z).
- A screen of 1280 × 800 or larger. A touch screen is recommended.

## 1. Install the package

Download the `.deb` for your architecture from the [GitHub releases](https://github.com/tinic/coffeecnc/releases), then install it:

```sh
sudo apt install ./coffeecnc_<version>_amd64.deb
```

The package installs:

| File | Use |
|---|---|
| `/usr/bin/coffeecnc` | The display program |
| `/usr/share/coffeecnc/coffeecnc.ini.example` | The INI keys CoffeeCNC reads, with comments |
| `/usr/share/coffeecnc/hal/coffeecnc_postgui.hal` | The HAL file that connects CoffeeCNC to LinuxCNC |

## 2. Change the INI file

Open the INI file of your machine. Make these changes:

1. In `[DISPLAY]`, set `DISPLAY = coffeecnc`. Add `--windowed` after the name to get a window instead of the full screen.
2. Add a `[COFFEECNC]` section. Copy the keys you need from `/usr/share/coffeecnc/coffeecnc.ini.example`. The minimum is:

   ```ini
   [COFFEECNC]
   MAX_SPINDLE_RPM = 24000
   MACHINE_ID = my-router
   MANUAL_TOOLCHANGE = yes
   ```

   - `MAX_SPINDLE_RPM`: the highest spindle speed the panel accepts. If you leave it out, CoffeeCNC uses `[SPINDLE_0] MAX_FORWARD_VELOCITY`.
   - `MACHINE_ID` names the machine’s data folder. If omitted, CoffeeCNC derives it from `[EMC] MACHINE`. Give each machine an explicit, stable ID if the names would produce the same folder.
   - `MANUAL_TOOLCHANGE = yes`: CoffeeCNC shows the tool change prompt and confirms the change to LinuxCNC. Remove `hal_manualtoolchange` or other tool change wiring from your HAL files first: CoffeeCNC connects only free pins, and does not start if another program owns them. With an automatic tool changer, or a tool change routine of your own, set `MANUAL_TOOLCHANGE = no`.

3. In `[HAL]`, set `POSTGUI_HALFILE = coffeecnc_postgui.hal`. A post-GUI file of another display connects pins of that display and does not load with CoffeeCNC.

## 3. Copy the HAL file

```sh
cp /usr/share/coffeecnc/hal/coffeecnc_postgui.hal ~/linuxcnc/configs/<your machine>/
```

The file loads the `watchdog` and `lut5` modules. LinuxCNC can load each module only once. If your HAL files already load `watchdog` or `lut5`, LinuxCNC stops with an error at this file. In that case:

- `lut5`: add `coffeecnc-jog-stop,coffeecnc-jog-inhibit,coffeecnc-eoffset-gate` to the `names=` list of your own `loadrt lut5` line. Remove the `loadrt lut5` line from the copy.
- `watchdog`: it has only one instance. Increase `num_inputs=` of your own `loadrt watchdog` line by one, and change `watchdog.input-0`, `watchdog.timeout-0` in the copy to the new input number. Remove the `loadrt watchdog` and its `addf` lines from the copy.

## 4. Start the machine

Start LinuxCNC from a terminal with your INI file. If CoffeeCNC refuses a value in the INI file, it does not start and writes the reason in the terminal, for example `[KINS] JOINTS must be 3.` Correct the value and start again.

## Optional functions

Each function stays off until you set its keys. The comments in `coffeecnc.ini.example` give every key and its unit. Copy an example HAL file beside your INI file, replace the signal names marked `MACHINE`, and `source` it after `coffeecnc_postgui.hal` from your post-GUI file.

| Function | Keys | HAL |
|---|---|---|
| Measured spindle speed and at-speed | `SPINDLE_RPM_SIGNAL`, `SPINDLE_AT_SPEED_SIGNAL` | Connect your signals to `coffeecnc.spindle-rpm` and `coffeecnc.spindle-at-speed` |
| Probe input display | `PROBE_SIGNAL` | Connect your probe signal to `coffeecnc.probe-input` |
| Touch probe in the spindle | `TOUCH_PROBE_TOOL` | Route the probe to `motion.probe-input` when this tool is loaded |
| Fixed tool setter | `FIXED_SETTER` and the `SETTER_*` keys | `/usr/share/coffeecnc/hal/examples/fixed_setter.hal`: probe input, setter selection and the probe-cycle permit; see [tool length probing](design/tool-length-probing.md) |
| Z jog during a pause | `PAUSED_Z_MAX_MM` | External offsets on Z, `[AXIS_Z] OFFSET_AV_RATIO`, and both spindle RPM and at-speed signals. See `/usr/share/coffeecnc/hal/examples/paused_z.hal`. |
| Travel check in Program check | `TRAVEL_MIN_MM`, `TRAVEL_MAX_MM` | None |

## Units

CoffeeCNC shows mm or inches; the operator selects them in the panel. The `[COFFEECNC]` keys that end in `_MM` or `_MM_MIN` are always in millimetres, also on an inch machine.

## An example

[The G0759 configuration](machines/g0759.md) shows a complete machine: a Grizzly G0759 mill with a Mesa 7i76e, a fixed tool setter, a touch probe and Z jog during a pause.
