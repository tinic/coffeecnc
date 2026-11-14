# Third-party geometry importer

`occt-import-js` 0.0.23 is installed as an exact npm dependency for STEP display geometry. It is an LGPL-2.1 JavaScript/WebAssembly interface to OpenCascade; it is not part of the controller implementation.

- [Importer source at the npm package's recorded revision](https://github.com/kovacsv/occt-import-js/tree/c2148e54b456b571238d35cac037d304053d64b2), including build instructions and its OCCT submodule.
- [OCCT source pinned by that revision](https://github.com/Open-Cascade-SAS/OCCT/tree/d2abb6d844231cb8f29be6894440874a4700e4a5).
- License texts supplied in the npm distribution are preserved as [importer license](public/licenses/occt-import-js.txt) and [OCCT license](public/licenses/occt.txt), with line endings and trailing whitespace normalized. Vite copies these into the built application.

The application does not yet load or distribute the WASM importer in its production bundle. When wiring it in, preserve the notices, corresponding source/build information and a way to replace/rebuild the library under its license. Record any local modifications. The research decision and current validation limits are in [STEP preview research](../docs/research/step-preview.md).

Bundled fonts have separate notices under [public/fonts](public/fonts/). Research samples retain their own [licenses and provenance](../samples/README.md).
