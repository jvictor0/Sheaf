# Delta — `synth-app-runtime`

A plugin host embeds the engine with no runtime of its own and keeps its state
in the DAW project. It names a patches root for its File page and no runtime
configuration file. `sar-8` and `sar-18` describe hosts that keep a
configuration; this requirement covers the host that keeps none.

## ADDED Requirements

### Requirement: sar-42 — Data paths: a host that keeps no runtime configuration
WHERE a host's runtime data paths name a patches root and no configuration file, THE engine SHALL load no runtime configuration, SHALL open no startup patch, and SHALL write no configuration file, at startup or after a patch command; a host whose configuration file is named but does not exist SHALL keep the startup behaviour of `sar-8` and `sar-18`.

#### Scenario: No startup patch without a configuration file
- **WHEN** the engine initializes, under a catalog that sets `patchCarriesMappings`, with a patches root holding a saved patch whose parameter values differ from the defaults and whose instrument section holds a controller, and with no configuration file named
- **THEN** after initialization and one message-thread tick, every parameter holds its default value and the live instrument equals the application's initial instrument
- Check: not yet delivered: NEW engine_initialize_without_a_configuration_file_opens_no_startup_patch in tests/engine_tests.cpp (frogg3rs task S1)

#### Scenario: A named but missing configuration keeps startup loading
- **WHEN** the engine initializes with a configuration file named but absent and a saved patch present
- **THEN** the newest patch opens
- Check: `tests/engine_tests.cpp`, `engine_initialize_treats_missing_runtime_config_as_defaults_and_still_loads_startup_patch`
