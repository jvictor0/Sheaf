# Browser catalog schema v1

The launcher accepts schema version `1`, browser ABI version `6`, UI protocol
version `2`, and runtime-config version `1`. This is a strict closed JSON
contract: every object must contain exactly its documented fields; missing or
unknown fields are rejected before an app is presented.

## Trusted discovery

`catalog-sources.json` is a nonempty array of catalog URLs configured by the
launcher deployment. Adding a URL is a host trust decision: an accepted
publisher supplies code that can execute after selection. Sources must be
HTTPS, except loopback HTTP for local tests; credentials and fragments are
rejected. The launcher fetches every registered catalog concurrently, retains
healthy catalogs when a sibling fails, and uses `no-cache` for **Retry
catalogs**.

## Catalog JSON

```json
{
  "schemaVersion": 1,
  "catalogVersion": "first-party-<digest-of-ordered-app-build-set>",
  "publisher": { "id": "example-labs", "name": "Example Labs" },
  "apps": [{
    "appId": "tone-grid",
    "displayName": "Tone Grid",
    "author": "Example Labs",
    "category": "Instrument",
    "buildId": "a-content-derived-immutable-id",
    "browser": {
      "abiVersion": 6,
      "uiProtocolVersion": 2,
      "runtimeConfigVersion": 1,
      "entry": "packages/tone-grid/a-content-derived-immutable-id/app.js",
      "files": [{
        "path": "packages/tone-grid/a-content-derived-immutable-id/app.js",
        "mediaType": "text/javascript",
        "size": 1234,
        "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
      }]
    }
  }]
}
```

`catalogVersion` is a trimmed 1--128-character string. For the first-party
publisher it is deterministically derived from the entire ordered
`{appId, buildId}` set, so it identifies the complete catalog publication, not
one app. `publisher.id`, `appId`, and `buildId` are lowercase kebab-case
identifiers. `apps` is nonempty with unique local app IDs. `buildId` identifies
an immutable build; it is not a mutable release label.

`browser` has exactly `abiVersion`, `uiProtocolVersion`,
`runtimeConfigVersion`, `entry`, and `files`; versions must match the host.
Paths are normalized catalog-relative paths. `files` is nonempty with unique
paths. Each record has `path`, `mediaType`, `size`, and `sha256`; accepted media
types are `text/javascript`, `application/wasm`, and
`application/octet-stream`. `entry` is a declared JavaScript file. The package
root is `packages/<app-id>/<build-id>/`; every listed file must remain inside
that immutable root.

Roles such as entry, Wasm, pthread worker, Wasm worker, and AudioWorklet are
assembler inputs, not catalog fields. Several roles may intentionally resolve
to one emitted bootstrap file. The full `files` inventory is authoritative.

## Runtime, identity, and audio

An app identity is `<publisher-id>/<app-id>`. Compatible packages are loaded
only after the host verifies every declared file's CORS response MIME, decoded
size, and SHA-256, materializes typed object URLs, and maps all Emscripten
sidecars explicitly. Integrity checks bytes; they are not a code sandbox.

Selection synchronously acquires one host `AudioContext` before package work.
The ABI-v4 generic module facade registers that same context with its
module-local `emscriptenRegisterAudioObject` helper, then starts native Wasm
AudioWorklet processing. Missing compatible context registration or native
startup is a launch failure before audio is online. There is no timer, animation
frame, message-loop, ScriptProcessor, or JavaScript sample-ring fallback.

Each package embeds its selected app and compatible Sheaf runtime. Patches live
under `patches/<publisher-id>/<app-id>`, which isolates identities across
builds. Only one app is active per navigation.

## Hosting, evidence, and rollback

Cloudflare is the cross-origin-isolated launcher host and includes response
headers, generic runtime modules, rollback pages, catalogs, and packages.
GitHub Pages is a publisher-only catalog/package origin and must provide public
CORS plus the exact declared JavaScript/Wasm MIME types. A local publication
test proves artifact and loopback behavior, not either service's live behavior.
The Pages CI workflow validates a deployed URL against the expected complete
`catalogVersion`, then checks every declared package response and runs a
deployed-origin smoke.

Republish a known-good Cloudflare artifact to roll back. Immutable package
paths and validated catalog artifacts are never edited in place. The
Cloudflare artifact also supplies one generic direct rollback page per catalog
app at `rollback/apps/<app-id>/`.
