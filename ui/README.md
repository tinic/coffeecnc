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

The 200 tests cover the protocol fixtures, simulator, interpreter, store, camera, units, jog lease, STEP mesh validation, model placement/import lifetime, parametric tool envelopes, default inheritance tool-model editor behavior, loaded-tool identity/position gating, capability-aware jog/execution controls, protocol-mismatch handling, program unload/reload identity, and partial text/directory presentation. Preview regressions cover changed initial position, inherited motion mode and prepared tool, while retaining a stable context during execution. The visual correction was also exercised in the browser: cycle start, manual tool acknowledgment, hold/resume, Stop, view switching while paused, and a held jog followed by release and zero feed. These are simulator checks, not machine validation.

The preview shows toolpaths, an optional STEP part layer and a configured loaded-tool envelope. Stock removal simulation, native WebKitGTK integration and actual LinuxCNC operation remain separate increments. Tool sequence labels come from interpreter segments and the tool table; they are not assumed CAM operations or claims of measurement. Fonts are bundled under `public/fonts/` with their licenses.

In Machine, use **Attach STEP model → Choose STEP file…**. The simulator also offers an SM-101 sample, explicitly labeled as not paired with the demo NC program. Import runs in a dedicated worker; Three.js renders a translucent mesh beneath the toolpath using the same orbit/pan/zoom camera. The importer and renderer load on demand from local assets. WebGL 2 is required for the optional part/tool meshes; the 2D toolpath remains available without it.

The model starts at the current work origin with placement marked unconfirmed. Edit XYZ translations in millimeters and XYZ rotations in degrees, then **Apply placement**; this changes only the display. Visibility and opacity are independent of the toolpath. Registration is captured against the program revision, WCS, G5x, G92 and XY rotation; changed work coordinates hide the model until placement is reviewed. Reloading/changing the program removes it. Attachments are page-local and are not saved across page reloads.

Imports are limited to 25 MB, 500,000 triangles and 800,000 vertices, with a 30-second deadline and cancellation. These are input/output limits, not a hard WASM heap quota. Failure preserves the previous attachment. Run `npm run check:step` to exercise the real WASM importer against all 29 repository STEP samples, including centimetre/inch source-unit checks against documented physical sizes. See [the library comparison and integration boundary](../docs/research/step-preview.md) and [third-party notices](THIRD_PARTY.md).

In Tools, **Model defaults** configures nominal proportions for seven tool families. **Set model** on a row selects a family and optional absolute dimension overrides, with a live WebGL 2 envelope preview. Family defaults persist in browser storage; per-tool assignments are page-local and invalidated by changed/deleted rows or a new connection hello. The editor supports mm/inch display, keeps physical dimensions independent of controller Z offsets, and does not issue controller commands. Production-browser checks covered saved defaults across reload, diameter scaling, overrides, the 1280 × 800 layout and a narrower viewport. See [the geometry and storage contract](../docs/design/tool-models.md). The Machine preview places the configured loaded tool at its reported tip and shares the STEP camera. Browser checks covered tool visibility, combined STEP/tool rendering and motion followed by feed hold at 1280 × 800. Native configuration persistence remains a subsequent increment.

Codex owns the visual components and styles; Claude owns the C++ backend and behavior review. See [implementation alignment](../docs/design/implementation-alignment.md) for the visual acceptance criteria. Representative 1280×800 captures are in `screenshots/`.

The additive v1 [machine-capability fields](../protocol/README.md) let the panel honor per-axis jog ceilings and an explicit homing requirement. Older peers keep the original behavior. Keyboard step/hold tests verify clamped outbound requests and release on a changed hello; simulator tests verify actual asymmetric jog movement and MDI/program homing gates. Optional spindle/probe feedback fields are declared but await native mapping and UI presentation.
