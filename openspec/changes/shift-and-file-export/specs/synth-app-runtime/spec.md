# Delta — `synth-app-runtime`

## ADDED Requirements

### Requirement: sar-33 — File export: an app hands a file to its host
WHEN a running app declares the optional file-export hook (`HasFileExports<App>`: `TakePendingFileExport()` returning an optional export of file name, media type, bytes and a note), THE synth system SHALL, once per message-thread tick after draining the app-action bus, take every pending export and pass it to the handler the host installed through `Engine::SetFileExportHandler`; with no handler installed it SHALL take the export and log its name rather than retain it. An app that declares no hook SHALL see this system entirely inert. The handler runs on the message thread and the audio thread SHALL never touch an export.

#### Scenario: A queued export reaches the installed handler once
- **WHEN** an app queues one export and the message thread ticks
- **THEN** the handler receives that export exactly once
- **AND** the app's queue is empty afterwards
- Check: `engine_tests.cpp: engine_delivers_a_queued_file_export_to_the_installed_handler_once`

#### Scenario: No handler drops the export with a log line, not a crash
- **WHEN** an app queues an export and no handler is installed
- **THEN** the tick takes it, logs its name, and the app's queue is empty
- Check: `engine_tests.cpp: engine_takes_a_file_export_with_no_handler_and_logs_it`
