# Third-party geometry importer

`occt-import-js` 0.0.23 is installed as an exact npm dependency for STEP display geometry. It is an LGPL-2.1 JavaScript/WebAssembly interface to OpenCascade; it is not part of the controller implementation.

- [Importer source at the npm package's recorded revision](https://github.com/kovacsv/occt-import-js/tree/c2148e54b456b571238d35cac037d304053d64b2), including build instructions and its OCCT submodule.
- [OCCT source pinned by that revision](https://github.com/Open-Cascade-SAS/OCCT/tree/d2abb6d844231cb8f29be6894440874a4700e4a5).
- License texts supplied in the npm distribution are preserved as [importer license](public/licenses/occt-import-js.txt) and [OCCT license](public/licenses/occt.txt), with line endings and trailing whitespace normalized. Vite copies these into the built application.

The production bundle includes the importer as a separate local WASM asset, loaded only when a model is selected. The unmodified npm loader/WASM can be replaced in `node_modules/occt-import-js/dist/` and the UI rebuilt with `npm run build`; upstream's source includes the Emscripten build instructions. Record any local modifications and preserve corresponding source/build information when packaging releases. Source references are also bundled in [SOURCES.txt](public/licenses/SOURCES.txt). The research decision and current validation limits are in [STEP preview research](../docs/research/step-preview.md).

The optional mesh renderer uses [Three.js](https://github.com/mrdoob/three.js) 0.187.0 under the [MIT license](public/licenses/three.txt). The simulator's SM-101 STEP example comes from the pinned [instant-machinist sample](../samples/upstream/instant-machinist/README.md) and retains its [MIT license](public/licenses/sm101-sample.txt). Neither this model nor its placement is a verified pairing with CoffeeCNC's demo toolpath.

Bundled fonts have separate notices under [public/fonts](public/fonts/). Research samples retain their own [licenses and provenance](../samples/README.md).
