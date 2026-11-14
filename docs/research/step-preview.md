# STEP geometry for the preview

Decision, 14 November 2026: use **`occt-import-js` 0.0.23**, pinned through npm in `ui/`. This is a JavaScript interface to C++ OpenCascade compiled as WebAssembly, not a STEP parser written in TypeScript. It provides import and tessellation; CoffeeCNC still needs to render and align the resulting mesh.

This increment installs the dependency and validates the existing STEP corpus. It does **not** add a model overlay to the operator panel yet.

## Candidates

| Library | What it supplies | Fit for CoffeeCNC |
| --- | --- | --- |
| [occt-import-js](https://github.com/kovacsv/occt-import-js) | Focused STEP/IGES/BREP import to triangle meshes, normals, colors and a node hierarchy. npm package; browser and Node support. | Selected for optional display geometry. Small API and an already-built WASM artifact let the browser simulator and native webview share the importer. |
| [OpenCascade.js](https://ocjs.org/docs/about) | A wider WebAssembly interface to the CAD kernel, including modeling operations. | More API surface than this overlay needs. Revisit if CoffeeCNC needs editable BREP geometry in the webview. |
| [Native OpenCascade / OCCT](https://github.com/Open-Cascade-SAS/OCCT/releases/tag/V8.0.1) | Current native C++ CAD kernel, STEP exchange and tessellation. | Strong alternative for a separate import helper, especially for newer kernel fixes or larger CAD workflows. It would add native build/packaging and mesh transport work. Do not link import work into the controller's command loop. |

This choice is about a display attachment. The C++ controller architecture stays intact. A rendering library is a separate decision: the existing preview uses a 2D canvas, so efficient shaded meshes will require a WebGL renderer or a deliberate replacement of that renderer. Merely installing a STEP importer does not provide the overlay.

## Version and distribution facts

- npm's latest published version on the research date is **0.0.23**, published 3 December 2024. The upstream repository's package file says 0.0.24; that version is not the published package we installed. The lockfile records the package integrity hash.
- npm records source revision [`c2148e54`](https://github.com/kovacsv/occt-import-js/tree/c2148e54b456b571238d35cac037d304053d64b2). Its OCCT submodule points to [`d2abb6d8`](https://github.com/Open-Cascade-SAS/OCCT/blob/d2abb6d844231cb8f29be6894440874a4700e4a5/src/Standard/Standard_Version.hxx), whose version header identifies **7.6.1**. This is materially older than native OCCT 8.0.1. Passing the corpus does not establish compatibility with every STEP exporter or incorporate newer kernel fixes.
- The installed WASM file is 7,604,031 bytes; its JavaScript loader is 96,871 bytes. Load them locally and on demand. No CDN or service is necessary.
- The package is **LGPL-2.1**, not MIT. Preserve its bundled importer and OCCT license texts and source references when packaging the application. See [third-party notices](../../ui/THIRD_PARTY.md). CoffeeCNC's own license remains undecided.
- The package does not ship TypeScript declarations. Add a small declaration for the API we actually use when implementing the worker; do not make the rest of the application depend on untyped importer objects.

Package metadata was checked with `npm view occt-import-js`; the API and output format are described in the [upstream README](https://github.com/kovacsv/occt-import-js/blob/c2148e54b456b571238d35cac037d304053d64b2/README.md).

## Corpus check

From the repository root:

```sh
npm ci --prefix ui
npm run check:step --prefix ui
```

The check executes the real pinned WASM module in Node. It requests millimeter output, 0.05 mm absolute linear deflection and 0.5 rad angular deflection. These are display settings, not machining tolerances.

| Sample | Meshes | Vertices | Triangles |
| --- | ---: | ---: | ---: |
| SM-101 `part.step` | 1 | 1,164 | 1,132 |
| Trebuchet left | 17 | 12,700 | 13,076 |
| Trebuchet right | 9 | 2,330 | 2,208 |

All three imports passed. The script checks finite coordinates/normals, triangle indices and hierarchy references. It repeats SM-101 in inches and compares converted bounds against millimeter output within 0.001 mm, and verifies that invalid input is rejected. SM-101 bounds were `[-60, -40, -20]` to `[60, 40, 0]` mm.

These results establish useful display output for these files, not complete BREP fidelity or agreement with the paired NC programs. Browser-worker loading, Vite asset bundling and the actual WebKitGTK runtime still need integration checks.

## Overlay implementation boundary

1. Associate a STEP attachment with an explicit program revision. Import in a dedicated worker with cancellation, a timeout and resource limits. `ReadStepFile` is synchronous after initialization; calling it on the page's event loop would freeze the controls. Terminating the worker is the cancellation mechanism.
2. Normalize geometry to millimeters and retain part names, colors and hierarchy where provided. Convert large arrays to transferable typed buffers. Report errors and empty geometry without disturbing the toolpath or controller state.
3. Store a model-to-work transform and the associated work coordinate system. Compose that with the same work-to-machine transform used for the toolpath. Neither filename similarity nor a shared file directory establishes alignment. Do not silently center or scale the CAD onto the path.
4. Render an independently toggleable, translucent **Part model** layer with opacity and alignment controls. Keep the toolpath visible. A CAD part is distinct from stock, fixtures and simulated remaining material.
5. Test on the target WebKitGTK build, including local WASM URLs, large imports, cancellation and program changes. A worker isolates CPU work from the UI thread; it is not a promise that webview memory exhaustion cannot affect the page. Native jog/stop handling must remain independent.

Keep a future native helper behind the same display-mesh contract if the older WASM kernel proves limiting. The [sample catalog](../../samples/README.md) records the existing model/program provenance and discrepancies.
