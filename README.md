<picture>
  <source media="(prefers-color-scheme: dark)" srcset="ui/public/branding/coffeecnc-logo-dark.svg">
  <img src="ui/public/branding/coffeecnc-logo-light.svg" alt="CoffeeCNC" width="360">
</picture>

CoffeeCNC is a touch-first operator panel for LinuxCNC milling machines. A native C++ application connects to LinuxCNC. A Vue/TypeScript interface runs inside a GTK/WebKit window on the machine controller.

LinuxCNC continues to own motion, interpretation and real-time control. CoffeeCNC gives the operator a clear screen for each step of the job: set up, measure tools, find the material, and run the program.

CoffeeCNC runs on a Grizzly **G0759** mill with a Mesa 7i76e, using the machine definitions in [tinic/linuxcnc](https://github.com/tinic/linuxcnc).

![CoffeeCNC Manual workspace on the G0759](ui/screenshots/g0759-manual.png)

## Features

### Manual

- Jog continuously or in steps from 0.005 mm. Jogging stops immediately when the touch probe or the tool setter makes contact. This is not an E-stop.
- Home each axis or all axes, enter MDI commands, and control the spindle and coolant.
- Large coordinates show work, machine or distance-to-go positions, in mm or inches.
- Numeric fields accept formulas such as `1/4in` or `7mm/2`. See [numeric entry](docs/numeric-entry.md).
- Each card shows its own status line, for example why MDI is not available now. Machine-wide states, such as "Machine is off", show once at the top.

### Tools

![Tools and work offsets on the G0759](ui/screenshots/g0759-tools.png)

- The tool table shows which tool lengths are measured and valid.
- **Touch off** measures the loaded tool on a fixed tool setter. Teach the setter once with the reference tool (T98), or with any tool that already has a valid length.
- **Load** confirms a manual tool change and applies its length offset. The last tool in the spindle comes back after a restart.
- Edit the origins of G54–G59.3 with a before/after confirmation.
- Each tool can have a 3D shape for the preview: end mills, ball-nose, bull-nose, chamfer and face mills, drills or a touch probe. See [the tool workflow](docs/design/tools-workflow.md) and [tool models](docs/design/tool-models.md).

### Touch off

![Material touch-off with the T99 probe on the G0759](ui/screenshots/g0759-touch-off.png)

- With the touch probe (T99) loaded, probe the **Z top** or one workpiece edge: **X−** (left), **X+** (right), **Y−** (front) or **Y+** (back). The diagram shows which face each button probes.
- **Corners** (X− Y−, X+ Y−, X− Y+, X+ Y+): probe Z top first, then jog the ball over the part within 10 mm of both edges. CoffeeCNC goes down beside each face and sets X0 and Y0 together.
- **Bore centre** and **Boss centre** set X0 and Y0 at the centre and show the diameter.
- **Edge angle** probes the front edge at two points and shows how far it turns, to square a vise.
- **Calibrate ball** measures the effective ball diameter on a gauge of known width and saves it to the tool table.
- Every move between contacts runs with the probe active, so an unexpected contact stops the machine. Nothing is written unless every contact succeeds.
- Results go into the active work coordinate system. Edge results include the probe ball radius.
- The probe can teach the tool setter, so you do not need to measure T98 first when the tool lengths are valid.

### Machine

![Running a program with the toolpath preview](ui/screenshots/machine-running-light.png)

- Open an NC program and look at the interpreter-generated toolpath before you run it. See [preview](docs/preview.md). Recently opened programs are listed first in **Open program**.
- The code list colors G-code by meaning, in both light and dark themes, and follows the running line.
- The tool sequence lists each section, its tool, its lines and its estimated time. The status line shows the estimated cycle time, then elapsed and remaining time during the run, and the last run time and part count.
- **Program check** lists the tools and their lengths, the work extents and lowest Z, feed and spindle ranges and the program units. Errors block Cycle start; warnings need a confirmation.
- **Restart** from any line or tool section. The restart sheet shows the state at that line and the approach: Z to clearance, the tool change if needed, X and Y, spindle and coolant, then a slow feed down. After a Stop, an E-stop or a power loss, the Machine tab offers **Restart there** at the line where the program stopped.
- **Run to line** pauses before a chosen line; Resume continues from there.
- **Air cut** runs the program with Z raised by a set amount, through a temporary offset that CoffeeCNC removes again.
- During a pause, jog **Z** up to inspect or clear chips. CoffeeCNC stops the spindle if needed, and Resume returns Z, restarts the spindle at speed, then continues.
- Tap an F or S number in the code list to change it. CoffeeCNC changes only that number in the file and opens the program again.
- Attach a STEP model to show the stock or part in the work coordinate system.
- Cycle start, feed hold, stop, and feed and spindle overrides are always at the bottom of the screen.
- [Air-cut programs](samples/air/README.md) let you try all of this without material.

<details>
<summary>Dark theme</summary>

![Running a program in the dark theme](ui/screenshots/machine-running-dark.png)

</details>

The G0759 screenshots come from the controller. The Machine screenshots come from the browser simulator.

## Try it in the browser

The browser simulator runs the full interface against a simulated machine. You do not need LinuxCNC.

```sh
cd ui
npm ci
npm run dev
```

Open [the simulated Machine workspace](http://127.0.0.1:5174/?scene=running&tab=program&theme=light). The simulator does not include the tool setter or the touch probe. See [UI development and checks](ui/README.md).

## Build and install

The native application builds with CMake and packages as a Debian package for the controller. See [the backend build](backend/README.md) and [packaging](docs/packaging.md). The tests include LinuxCNC simulation scenarios for jogging, tool setting and probing.

## Design

- A native C++ application owns the LinuxCNC connection and the command lifecycle. Separate command, status and HAL threads keep jog-stop handling independent of blocked NML commands.
- GTK/WebKit hosts the local web UI through an in-process message bridge.
- Plain G-code is a first-class input. STEP models, setup sheets and CAM metadata add to a job when they are available.
- Machine settings and HAL integration stay separate from the shared application code. See [the G0759 configuration](docs/machines/g0759.md).

More: [architecture](docs/architecture.md), [native shell](docs/shell.md), [product brief](docs/product-brief.md), [machinist workflows](docs/research/machinist-workflows.md), [sample catalog](samples/README.md), [logo and identity](docs/design/branding/README.md).

## License

A project-wide license is not selected yet. Third-party sample files keep their own upstream licenses and attribution.
