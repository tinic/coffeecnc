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

The 154 tests cover the protocol fixtures, simulator, interpreter, store, camera, units and jog lease. The visual correction was also exercised in the browser: cycle start, manual tool acknowledgment, hold/resume, Stop, view switching while paused, and a held jog followed by release and zero feed. These are simulator checks, not machine validation.

The preview currently shows toolpaths. Associated CAD/stock geometry, native WebKitGTK integration and actual LinuxCNC operation remain separate increments. Tool sequence labels come from interpreter segments and the tool table; they are not assumed CAM operations or claims of measurement. Fonts are bundled under `public/fonts/` with their licenses.

`occt-import-js` is pinned for the upcoming STEP overlay. Run `npm run check:step` to exercise its real WASM importer against all three repository STEP samples, including mesh integrity and output-unit checks. See [the library comparison and integration boundary](../docs/research/step-preview.md) and [third-party notices](THIRD_PARTY.md). The dependency is installed; the panel does not load it yet.

Codex owns the visual components and styles; Claude owns the C++ backend and behavior review. See [implementation alignment](../docs/design/implementation-alignment.md) for the visual acceptance criteria. Representative 1280×800 captures are in `screenshots/`.
