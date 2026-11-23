# Set up CoffeeCNC on your machine

This guide puts CoffeeCNC on a LinuxCNC machine that already runs with another display, for example AXIS or Gmoccapy. Keep a copy of your configuration folder before you start.

## Requirements

- Debian 13 (trixie), amd64 or arm64.
- LinuxCNC 2.9 (`linuxcnc-uspace`) from the linuxcnc.org `trixie 2.9-uspace` repository. Each CoffeeCNC package is built for one LinuxCNC version, given in the release notes and in the package's dependency. After you update LinuxCNC, install the CoffeeCNC package built for the new version.
- A milling machine with X, Y and Z axes and `trivkins` (joint 0 is X, joint 1 is Y, joint 2 is Z).
- A gantry router is also supported: two motors on one axis, for example `COORDINATES = X Y Y Z` with `KINEMATICS = trivkins coordinates=XYYZ` (`kinstype=both` is accepted). Give the two joints of the axis the same negative `HOME_SEQUENCE`, as pncconf does, so that they home and square together.
- A screen of 1280 × 800 or larger. A touch screen is recommended.
- Languages: choose English, French, German, Spanish, Portuguese, Chinese or Japanese in **Settings**. Chinese and Japanese need CJK fonts: the package recommends `fonts-noto-cjk`, which apt installs by default. If you install with `--no-install-recommends`, also run `sudo apt install fonts-noto-cjk`.

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
   - `MACHINE_ID`: the name of the folder where CoffeeCNC saves the data of this machine: tool models, the tool in the spindle, touched-off tools and the restart point. If you leave it out, CoffeeCNC makes a name from `[EMC] MACHINE` and the location of the INI file. Set a name if you move or copy the configuration folder, so that CoffeeCNC keeps using the same data.
   - `DATA_DIRECTORY` (optional): an absolute path for the data folders. The default is `~/.local/share/coffeecnc` (or `$XDG_DATA_HOME/coffeecnc`). CoffeeCNC versions before 0.1 kept some of this data in the configuration folder; the first start moves it.
   - `MANUAL_TOOLCHANGE = yes`: CoffeeCNC shows the tool change prompt and confirms the change to LinuxCNC. Remove `hal_manualtoolchange` or other tool change wiring from your HAL files first: CoffeeCNC connects only free pins, and does not start if another program owns them. With an automatic tool changer, or a tool change routine of your own, set `MANUAL_TOOLCHANGE = no`.

3. In `[HAL]`, set `POSTGUI_HALFILE = coffeecnc_postgui.hal`. A post-GUI file of another display connects pins of that display and does not load with CoffeeCNC.

## 3. Copy the HAL file

```sh
cp /usr/share/coffeecnc/hal/coffeecnc_postgui.hal ~/linuxcnc/configs/<your machine>/
```

The file loads the `watchdog` and `lut5` modules. LinuxCNC can load each module only once. If your HAL files already load `watchdog` or `lut5`, LinuxCNC stops with an error at this file. In that case:

- `lut5`: add `coffeecnc-jog-stop,coffeecnc-jog-inhibit,coffeecnc-eoffset-gate` to the `names=` list of your own `loadrt lut5` line. Remove the `loadrt lut5` line from the copy.
- `watchdog`: it has only one instance. Increase `num_inputs=` of your own `loadrt watchdog` line by one, and change `watchdog.input-0`, `watchdog.timeout-0` in the copy to the new input number. Remove the `loadrt watchdog` and its `addf` lines from the copy.

## 4. Check the INI file

```sh
coffeecnc --check ~/linuxcnc/configs/<your machine>/<your machine>.ini
```

The check tells you if CoffeeCNC starts with the file and, if not, why. It also lists the optional functions, if each one is on or off, and the keys that switch it on. LinuxCNC does not need to run.

## 5. Start the machine

Start LinuxCNC from a terminal with your INI file. If CoffeeCNC refuses a value in the INI file, it does not start and writes the reason in the terminal, for example `[KINS] JOINTS must be 3.` Correct the value and start again.

## Optional functions

Each function stays off until you set its keys. The comments in `coffeecnc.ini.example` give every key and its unit. Copy an example HAL file beside your INI file, replace the signal names marked `MACHINE`, and `source` it after `coffeecnc_postgui.hal` from your post-GUI file.

| Function | Keys | HAL |
|---|---|---|
| Measured spindle speed and at-speed | `SPINDLE_RPM_SIGNAL`, `SPINDLE_AT_SPEED_SIGNAL` | Connect your signals to `coffeecnc.spindle-rpm` and `coffeecnc.spindle-at-speed` |
| Probe input display | `PROBE_SIGNAL` | Connect your probe signal to `coffeecnc.probe-input` |
| Reference marks on the Home buttons | `REFERENCE_MARKS` | For a machine that homes on reference marks its HAL has to find first. For each of X, Y and Z connect a bit that is true once the axis's mark is known to `coffeecnc.reference-mark-x`, `-y`, `-z`, and a bit that is true while the axis stands at its mark to `coffeecnc.at-reference-mark-x`, `-y`, `-z`. See [reference marks](#reference-marks) |
| Touch probe in the spindle | `TOUCH_PROBE_TOOL` | Route the probe to `motion.probe-input` when this tool is loaded |
| Fixed tool setter | `FIXED_SETTER` and the `SETTER_*` keys | `/usr/share/coffeecnc/hal/examples/fixed_setter.hal`: probe input, setter selection and the probe-cycle permit; see [tool length probing](design/tool-length-probing.md) |
| Z jog during a pause | `PAUSED_Z_MAX_MM`, and also `SPINDLE_RPM_SIGNAL` and `SPINDLE_AT_SPEED_SIGNAL` | `/usr/share/coffeecnc/hal/examples/paused_z.hal`: external offsets on Z, with `[AXIS_Z] OFFSET_AV_RATIO` in the INI file |
| Travel check in Program check | `TRAVEL_MIN_MM`, `TRAVEL_MAX_MM` | None |
| Macro buttons in MDI | `MACRO` | None. See [macros](#macros) |

## Macros

A macro is a button that runs one MDI command. Add one `MACRO` line for each macro to `[COFFEECNC]`, with the label, a `|` and the MDI command:

```ini
[COFFEECNC]
MACRO = Park | G53 G0 Z0
MACRO = Clamp check | O<clamp> call
```

- You can set at most 12 macros. A label has at most 24 characters. Each macro must have a different label.
- The MDI command is one line of at most 254 bytes.
- For a macro with more steps, write the steps in a subroutine file and call it with `O<name> call`. Put `name.ngc` in `[DISPLAY] PROGRAM_PREFIX` or in a directory of `[RS274NGC] SUBROUTINE_PATH`. If CoffeeCNC cannot find the file, it does not start and tells you which macro is wrong.

In Manual, the MDI card then shows a **Macros** button. Tap it, then tap a macro. CoffeeCNC shows the MDI command. Tap **Run** to send it. The macro has the same conditions as an MDI command that you type: for example, the machine must be on, homed if your configuration requires it, and idle. The result shows in the MDI history. Without `MACRO` lines, the panel shows no **Macros** button. `coffeecnc --check` lists the macros.

## Reference marks

A linear scale with a single reference mark cannot tell, after power-up, on which side of the mark its axis stands. A machine that homes on such marks has HAL logic that learns where a mark is when the axis crosses it. `[COFFEECNC] REFERENCE_MARKS = yes` lets the panel show that state and keeps Home from starting too early. It is all three axes or none.

| Pin of the `coffeecnc` component | Direction | Your HAL sets it | CoffeeCNC does |
|---|---|---|---|
| `coffeecnc.reference-mark-x`, `-y`, `-z` | bit in | True once the mark of the axis is known, so that homing can go to it | While false: the Home button of the axis reads "find mark", and Home for that axis and Home all are refused with "Move X across its reference mark first: homing cannot find it yet." |
| `coffeecnc.at-reference-mark-x`, `-y`, `-z` | bit in | True while the axis stands at its mark, within a distance you choose | The letter on the Home button of the axis is red and the button reads "at mark", until the axis is homed. This works with the machine off, so that an axis moved by hand can be stopped at its mark |

Connect the six pins in your post-GUI HAL file, after `coffeecnc_postgui.hal`. The pins exist whatever the key says; without `REFERENCE_MARKS = yes` they are not read, the Home buttons look as on any machine and Home is never refused for this reason. With the key set, a pin you leave unconnected reads false: its axis stays at "find mark".

The homing move itself is LinuxCNC's, configured in `[JOINT_n]` of your INI file; CoffeeCNC sends the same home command as on a machine with switches. [The G0759 configuration](machines/g0759.md#reference-mark-homing-22-november-2026) is a complete example, with the HAL component that derives a home switch from the mark.

## Units

CoffeeCNC shows mm or inches; the operator selects them in the panel. The `[COFFEECNC]` keys that end in `_MM` or `_MM_MIN` are always in millimetres, also on an inch machine.

## An example

[The G0759 configuration](machines/g0759.md) shows a complete machine: a Grizzly G0759 mill with a Mesa 7i76e, a fixed tool setter, a touch probe and Z jog during a pause.
