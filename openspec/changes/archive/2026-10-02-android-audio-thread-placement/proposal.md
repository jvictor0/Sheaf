# Proposal — `android-audio-thread-placement`

## Why

On a Galaxy S20 FE (Snapdragon 865: four 1.8 GHz Cortex-A55, four 2.4-2.84
GHz Cortex-A77, Android 13) Android runs frogg3rs's real-time audio thread
(`AAudio_1`, SCHED_FIFO, MMAP, 48-frame bursts) on an A55, where it used
98.5% of the core with the transport stopped: DSP pinned at 100%, playback
slowing and skipping. The same build with that thread moved onto the A77s
used 76.7% of one, inside its deadline. Nothing else measured on the phone
moved it.

## What changes

On Android, `Runtime`'s audio callback moves its own thread onto the
device's faster cores the first time each audio thread runs it. The set of
faster cores (every core whose maximum frequency is above the slowest core's)
is read from `/sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq` on the
message thread in `audioDeviceAboutToStart`; the callback only applies it,
with no file access, allocation or logging, and records the attempt even if
it fails so it is never retried there. A core whose file cannot be read
(offline, or denied) is skipped; with fewer than two distinct readable
frequencies nothing changes.
Other platforms are untouched.

## Impact

`projects/synth/runtime/Runtime.hpp`.

## Delivery

On `phone-width-composition-and-record-permission` (jvictor0/Sheaf#25), whose
scope is the Android host.
