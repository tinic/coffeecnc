# STEP geometry for the preview

Decision, 14 November 2026: use **`occt-import-js` 0.0.23**, pinned through npm in `ui/`. This is a JavaScript interface to C++ OpenCascade compiled as WebAssembly, not a STEP parser written in TypeScript. It provides import and tessellation; CoffeeCNC still needs to render and align the resulting mesh.

The follow-up implements local STEP selection, cancellable worker import, an optional translucent Three.js layer and explicit placement controls. Model state is local to the page; persistent job attachments and native WebKitGTK qualification remain open.

## Candidates

| Library | What it supplies | Fit for CoffeeCNC |
| --- | --- | --- |
| [occt-import-js](https://github.com/kovacsv/occt-import-js) | Focused STEP/IGES/BREP import to triangle meshes, normals, colors and a node hierarchy. npm package; browser and Node support. | Selected for optional display geometry. Small API and an already-built WASM artifact let the browser simulator and native webview share the importer. |
| [OpenCascade.js](https://ocjs.org/docs/about) | A wider WebAssembly interface to the CAD kernel, including modeling operations. | More API surface than this overlay needs. Revisit if CoffeeCNC needs editable BREP geometry in the webview. |
| [Native OpenCascade / OCCT](https://github.com/Open-Cascade-SAS/OCCT/releases/tag/V8.0.1) | Current native C++ CAD kernel, STEP exchange and tessellation. | Strong alternative for a separate import helper, especially for newer kernel fixes or larger CAD workflows. It would add native build/packaging and mesh transport work. Do not link import work into the controller's command loop. |

This choice is about a display attachment. The C++ controller architecture stays intact. Three.js 0.187.0 now supplies a WebGL 2 layer beneath the existing 2D toolpath canvas. Both use the same orthographic projection, covered by pixel-coordinate tests for named views, orbit, pan and zoom. Keeping the toolpath on top makes it visible through the part; this is a reference overlay, not an occlusion or collision simulation.

## Version and distribution facts

- npm's latest published version on the research date is **0.0.23**, published 3 December 2024. The upstream repository's package file says 0.0.24; that version is not the published package we installed. The lockfile records the package integrity hash.
- npm records source revision [`c2148e54`](https://github.com/kovacsv/occt-import-js/tree/c2148e54b456b571238d35cac037d304053d64b2). Its OCCT submodule points to [`d2abb6d8`](https://github.com/Open-Cascade-SAS/OCCT/blob/d2abb6d844231cb8f29be6894440874a4700e4a5/src/Standard/Standard_Version.hxx), whose version header identifies **7.6.1**. This is materially older than native OCCT 8.0.1. Passing the corpus does not establish compatibility with every STEP exporter or incorporate newer kernel fixes.
- The installed WASM file is 7,604,031 bytes; its JavaScript loader is 96,871 bytes. Load them locally and on demand. No CDN or service is necessary.
- The package is **LGPL-2.1**, not MIT. Preserve its bundled importer and OCCT license texts and source references when packaging the application. See [third-party notices](../../ui/THIRD_PARTY.md). CoffeeCNC's own license remains undecided.
- The package does not ship TypeScript declarations. CoffeeCNC declares the narrow API it uses and validates the returned mesh arrays in the worker before transferring typed buffers to the page.

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

All three initial imports passed. The script checks finite coordinates/normals, triangle indices and hierarchy references, repeats the first fixture in inches and compares converted bounds against millimeter output within 0.001 mm, and verifies that invalid input is rejected. SM-101 bounds were `[-60, -40, -20]` to `[60, 40, 0]` mm.

The expanded corpus check now discovers every STEP file in `samples/manifest.json`: **all 29 models passed** on 2026-11-14. It additionally compares three Camcraft models against the physical dimensions documented in the upstream fixture README. The centimetre-authored `ops_block.step` and `ops_2d.step` import at 100 × 70 × 20 mm and 140 × 100 × 20 mm; the inch-authored `lego_4x2_01.step` imports at 100.584 × 49.784 × 38.1 mm. This checks source-unit conversion independently of the output-unit round trip. Passing mesh integrity and size checks does not establish STEP/NC registration or complete B-rep face coverage.

These results establish useful display output for these files, not complete BREP fidelity or agreement with the paired NC programs. Browser-worker loading and Vite asset bundling are exercised separately in the operator panel. The actual WebKitGTK runtime still needs qualification.

## Implemented overlay and remaining boundaries

1. Attachments bind to the loaded program path/revision. A dedicated worker handles each import and is terminated on completion, cancellation, a program change or the 30-second deadline. Limits are 25 MB input, 500,000 output triangles, 800,000 vertices and 4,096 meshes. These do not impose a hard limit on the importer's intermediate memory.
2. Output is normalized to millimeters. The worker validates finite coordinates, normals, colors and indices before transferring typed buffers. Mesh names and mesh colors are retained; assembly browsing and per-face colors are not exposed in this first renderer. Failed imports retain the previous attachment.
3. Registration rotates the CAD about X, then Y, then Z, adds an explicit translation in work millimeters, then applies G92, the work frame's XY rotation and G5x. It does not add the tool offset. A captured work frame prevents a changed WCS/origin from silently moving the model. The layer is hidden until the operator reviews placement against the new frame. Neither filenames nor directory proximity establish alignment.
4. Visibility and opacity are independent of toolpaths. The initial placement is explicitly unconfirmed. Applying placement records an operator declaration, not a verification. The bundled SM-101 example is labeled as not paired with the demo program. Attachments are lost on page reload and removed on program reload/change.
5. WebGL failure leaves the 2D toolpath available. The target WebKitGTK build still needs local WASM, file-picker, graphics and large-model checks. A worker isolates CPU work from the UI thread; it does not guarantee that webview memory exhaustion cannot affect the page. Native jog/stop handling remains independent.

Keep a future native helper behind the same display-mesh contract if the older WASM kernel proves limiting. The [sample catalog](../../samples/README.md) records the existing model/program provenance and discrepancies.
