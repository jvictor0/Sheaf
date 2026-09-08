# Delta — `synth-browser-wasm-runtime`

## ADDED Requirements

### Requirement: sbw-12 — File export: a download under the app's name
WHEN an app running in the browser runtime hands the engine a file export, THE browser runtime SHALL queue it and expose it through a `synth_browser_dequeue_file_export` ABI entry returning the name, media type and bytes in the pointer-and-size shape the UI frame uses, valid until the next dequeue; THE worker SHALL drain that queue after each message tick and post each export to the page as an unsolicited `file-export` response with the bytes transferred; and THE page SHALL offer it as a download under the export's file name and media type, releasing the object URL afterwards. Nothing about an export SHALL be written to the runtime's persisted storage.

#### Scenario: The ABI returns exports in order and then nothing
- **WHEN** two exports are queued and the entry is called three times
- **THEN** it returns the first, the second, and then reports none
- Check: `browser_runtime_contract_tests.cpp: TestBrowserRuntimeDequeuesQueuedFileExportsInOrder`

#### Scenario: A fixture app's export becomes a download
- **WHEN** the fixture app's export action is clicked in a Playwright run
- **THEN** the page emits a download whose suggested name, type and bytes are the fixture's
- Check: `browser/tests/file-export.spec.ts`, driving one message tick after the click
  since the fixture harness never ticks on its own
