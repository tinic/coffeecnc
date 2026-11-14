# CoffeeCNC

A new milling frontend for LinuxCNC, with a native C++ core and a modern web interface hosted locally in GTK/WebKit.

CoffeeCNC is an independent product. It shares architectural ideas with [Elle](https://github.com/tinic/el-linuxcnc-electron), the electronic leadscrew project, but has its own milling workflows and product scope.

**Current status: design and workflow research.** The interactive concept is a visual prototype with simulated values. There is no CoffeeCNC controller backend yet.

## Explore the concept

Open [the standalone concept](docs/concepts/coffeecnc-mill.html) in a browser after cloning. [The editable fragment](docs/concepts/coffeecnc-mill.fragment.html) is also included. The prototype uses external font and icon resources; it is not an offline production application.

The concept explores readable coordinates, a part/toolpath view, operation context, and persistent run controls. It is a starting point for discussion: real jogging, touch-off, tool changes, proving a program, and recovery still need workflow design. Its displayed geometry, measurements, tool readiness and timing are illustrative.

## Direction

- A native C++ application owns the LinuxCNC connection and command lifecycle.
- GTK/WebKit hosts a local web UI through an in-process message bridge.
- LinuxCNC continues to own motion, interpretation and real-time control.
- Plain G-code is a first-class input. STEP, setup sheets and CAM metadata enrich a job when available.
- Machine state, operator declarations and preview assumptions remain distinguishable.

See [the product brief](docs/product-brief.md) and [material provenance](docs/provenance.md). Workflow research and a licensed sample corpus are being added in follow-up commits.

## License status

A project-wide license for CoffeeCNC has not yet been selected. Third-party sample files retain their own upstream licenses and attribution; they are not relicensed as CoffeeCNC.
