# CoffeeCNC operator panel

Vue 3 / TypeScript panel using the [original CoffeeCNC concept](../docs/concepts/coffeecnc-mill.html) as its visual base. The browser simulator is separate from LinuxCNC and is labeled throughout the interface.

```sh
npm ci
npm run dev
```

Open `http://127.0.0.1:5174/?scene=running&tab=program&theme=light`. Use `theme=dark` for the other theme. `tab=manual` and `tab=tools` select the other workspaces; view changes do not request a machine mode change. The default view of a program is its toolpath, with Code and Split available explicitly.

```sh
npm run check
npm run build
```

The 166 tests cover the protocol fixtures, simulator, interpreter, store, camera, units, jog lease, STEP mesh validation, model placement and import lifetime. The visual correction was also exercised in the browser: cycle start, manual tool acknowledgment, hold/resume, Stop, view switching while paused, and a held jog followed by release and zero feed. These are simulator checks, not machine validation.

The preview shows toolpaths and an optional STEP part layer. Stock removal simulation, native WebKitGTK integration and actual LinuxCNC operation remain separate increments. Tool sequence labels come from interpreter segments and the tool table; they are not assumed CAM operations or claims of measurement. Fonts are bundled under `public/fonts/` with their licenses.

In Machine, use **Attach STEP model → Choose STEP file…**. The simulator also offers an SM-101 sample, explicitly labeled as not paired with the demo NC program. Import runs in a dedicated worker; Three.js renders a translucent mesh beneath the toolpath using the same orbit/pan/zoom camera. The importer and renderer load on demand from local assets. WebGL 2 is required only for the part layer.

The model starts at the current work origin with placement marked unconfirmed. Edit XYZ translations in millimeters and XYZ rotations in degrees, then **Apply placement**; this changes only the display. Visibility and opacity are independent of the toolpath. Registration is captured against the program revision, WCS, G5x, G92 and XY rotation; changed work coordinates hide the model until placement is reviewed. Reloading/changing the program removes it. Attachments are page-local and are not saved across page reloads.

Imports are limited to 25 MB, 500,000 triangles and 800,000 vertices, with a 30-second deadline and cancellation. These are input/output limits, not a hard WASM heap quota. Failure preserves the previous attachment. Run `npm run check:step` to exercise the real WASM importer against all three repository STEP samples. See [the library comparison and integration boundary](../docs/research/step-preview.md) and [third-party notices](THIRD_PARTY.md).

Codex owns the visual components and styles; Claude owns the C++ backend and behavior review. See [implementation alignment](../docs/design/implementation-alignment.md) for the visual acceptance criteria. Representative 1280×800 captures are in `screenshots/`.
