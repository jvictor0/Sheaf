# Delta — synth-app-runtime

## ADDED Requirements

### Requirement: sar-33 — Audio: external-input-routed signal with change notification

THE runtime SHALL expose to apps, via `AppContext`, an explicit
external-input-routed signal meaning "a user-chosen input source is open and
delivering", with a registered change callback invoked on the message thread
when the value changes.

On the JUCE host, routed is true only while the user-selected input device is
non-empty and is the open device; a platform-default input device opened
without user selection reports not-routed. On the browser host, routed is
true only while user-gesture-granted input capture is active. Reads are safe
from the audio thread.

#### Scenario: default-opened device is not routed

- **WHEN** the host auto-opens the platform default input and the user has
  selected no input device
- **THEN** the signal reads false, even though the device presents an active
  input channel

#### Scenario: user selection flips the signal live

- **WHEN** the user selects an input device on the Audio page
- **THEN** the signal reads true and the registered callback fires on the
  message thread without an app restart; deselecting reverses both

#### Scenario: browser capture gates the signal

- **WHEN** the browser host runs without user-gesture-granted input capture
- **THEN** the signal reads false; granting capture flips it true and fires
  the callback

### Requirement: sar-34 — Packaging: bundle executable and plist coherence

WHEN the bundle rule copies `APP_INFO_PLIST`, THE build SHALL fail with a
message naming both values if the plist's `CFBundleExecutable` does not equal
`APP_NAME`, so a mis-bundle that would only fail at Finder launch becomes a
build error.

#### Scenario: mismatch fails the build

- **WHEN** the plist's `CFBundleExecutable` differs from `APP_NAME`
- **THEN** the bundle rule exits non-zero naming the plist value and the
  expected name, and no bundle is produced

#### Scenario: coherent bundle builds

- **WHEN** the plist's `CFBundleExecutable` equals `APP_NAME`
- **THEN** the bundle builds exactly as before this change
