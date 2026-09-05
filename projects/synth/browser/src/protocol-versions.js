// The three version numbers, in a module Node can load from source.
//
// protocol.ts re-exports these, so there is still exactly one literal per
// version and every TypeScript consumer keeps importing them from there. They
// live here because static-server.mjs needs them and is a .mjs: it cannot
// import a .ts at runtime, and reaching for the compiled protocol.js instead
// inverted the dependency -- source asking for build output. That inversion is
// what made the file's behaviour depend on which tree it was loaded from, and
// it took the published site down when the compiled copy looked one directory
// too deep. A sibling .js is copied into dist/src/ beside its callers, so it
// resolves the same from either tree and needs nothing built first.
export const SUPPORTED_BROWSER_ABI_VERSION = 6;
// Version 2 (sru-46): node bounds are parent-relative, `Draw` geometry is
// node-local, node colour/text style and container border fields cross the
// wire behind explicit presence bytes, and `variant` is gone. A hard break
// with strict equality on both ends and no version-1 fallback. Moves in
// lockstep with C++ `kCommandBufferVersion` and each Wasm package's exported
// `synth_browser_ui_protocol_version()`.
export const SUPPORTED_UI_PROTOCOL_VERSION = 2;
export const SUPPORTED_RUNTIME_CONFIG_VERSION = 1;
