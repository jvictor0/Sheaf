#include "synth/browser/BrowserRuntime.hpp"

extern "C" synth_browser::RuntimeAbi* synth_browser_create_runtime();

namespace {

synth_browser::RuntimeAbi* RuntimeFor(synth_browser_runtime* handle)
{
    return reinterpret_cast<synth_browser::RuntimeAbi*>(handle);
}

}  // namespace

// v4 widened the audio input status range from 0-7 to 0-10 so the browser host
// can name a missing secure context, a permissions-policy block, and a missing
// launch-owned AudioContext individually (sbw-10). A v3 module rejects those
// codes, so the launcher must not be able to hand them to one.
//
// v6 renamed `synth_browser_consume_pending_audio_input_request` to
// `synth_browser_consume_pending_audio_request` and added an `outControl`
// parameter, generalizing the one pending-request slot to also carry an
// audio output selection alongside input retry/select. A v5 module exports
// the old one-argument symbol under the old name; a host built against v6
// would call a symbol that either does not exist on that module or, if it
// happened to share the old name, would be called with an extra argument the
// module never reads. The version gate keeps that mismatch from ever being
// attempted.
extern "C" std::uint32_t synth_browser_abi_version()
{
    return 6;
}

// Every Wasm package exports this independently of the shell bundle, so it must
// equal the shell's `COMMAND_BUFFER_VERSION`; a package still advertising 1
// against a version-2 shell is rejected before any frame renders (sru-46).
extern "C" std::uint32_t synth_browser_ui_protocol_version()
{
    return 2;
}

extern "C" std::uint32_t synth_browser_runtime_config_version()
{
    return synth_browser::kBrowserRuntimeConfigVersion;
}

extern "C" synth_browser_runtime* synth_browser_create()
{
    return reinterpret_cast<synth_browser_runtime*>(synth_browser_create_runtime());
}

extern "C" int synth_browser_initialize(synth_browser_runtime* runtime, const char* publisherId,
                                          const char* appId, std::uint32_t runtimeConfigVersion)
{
    return RuntimeFor(runtime) == nullptr
               ? -1
               : RuntimeFor(runtime)->Initialize(publisherId, appId, runtimeConfigVersion);
}

extern "C" std::size_t synth_browser_audio_output_channels(synth_browser_runtime* runtime)
{
    return RuntimeFor(runtime) == nullptr ? 0 : RuntimeFor(runtime)->AudioOutputChannels();
}

extern "C" std::size_t synth_browser_audio_input_channels(synth_browser_runtime* runtime)
{
    return RuntimeFor(runtime) == nullptr ? 0 : RuntimeFor(runtime)->AudioInputChannels();
}

extern "C" int synth_browser_prepare(synth_browser_runtime* runtime, double sampleRate, std::size_t blockSize)
{
    return RuntimeFor(runtime) == nullptr ? -1 : RuntimeFor(runtime)->Prepare(sampleRate, blockSize);
}

extern "C" int synth_browser_process(synth_browser_runtime* runtime, float** outputs, std::size_t outputChannels,
                                       std::size_t frames, std::uint64_t timestampMicros)
{
    return RuntimeFor(runtime) == nullptr ? -1 : RuntimeFor(runtime)->Process(outputs, outputChannels, frames,
                                                                               timestampMicros);
}

extern "C" int synth_browser_start_audio_worklet(synth_browser_runtime* runtime,
                                                  std::uint32_t audioContextHandle)
{
    return RuntimeFor(runtime) == nullptr
               ? -1
               : RuntimeFor(runtime)->StartAudioWorklet(audioContextHandle);
}

extern "C" int synth_browser_set_audio_input_source(synth_browser_runtime* runtime,
                                                     std::uint32_t sourceHandle,
                                                     std::uint32_t physicalChannels,
                                                     std::uint32_t statusCode)
{
    return RuntimeFor(runtime) == nullptr
               ? -1
               : RuntimeFor(runtime)->SetAudioInputSource(sourceHandle, physicalChannels, statusCode);
}

extern "C" int synth_browser_clear_audio_input_source(synth_browser_runtime* runtime,
                                                       std::uint32_t statusCode)
{
    return RuntimeFor(runtime) == nullptr
               ? -1
               : RuntimeFor(runtime)->ClearAudioInputSource(statusCode);
}

extern "C" int synth_browser_consume_pending_audio_request(synth_browser_runtime* runtime,
                                                             std::uint32_t* outControl)
{
    if (RuntimeFor(runtime) == nullptr) {
        if (outControl != nullptr) {
            *outControl = 0;
        }
        return synth_browser::kNoPendingAudioRequest;
    }
    return RuntimeFor(runtime)->ConsumePendingAudioRequest(outControl);
}

extern "C" int synth_browser_set_timestamp_epoch_offset(
    synth_browser_runtime* runtime, std::int64_t offsetMicros)
{
    return RuntimeFor(runtime) == nullptr
               ? -1
               : RuntimeFor(runtime)->SetTimestampEpochOffsetMicros(offsetMicros);
}

extern "C" std::uint32_t synth_browser_audio_worklet_block_count(synth_browser_runtime* runtime)
{
    return RuntimeFor(runtime) == nullptr ? 0 : RuntimeFor(runtime)->AudioWorkletBlockCount();
}

extern "C" std::uint32_t synth_browser_audio_worklet_peak_microunits(synth_browser_runtime* runtime)
{
    return RuntimeFor(runtime) == nullptr ? 0 : RuntimeFor(runtime)->AudioWorkletPeakMicrounits();
}

extern "C" std::uint32_t synth_browser_audio_worklet_deadline_microunits(synth_browser_runtime* runtime)
{
    return RuntimeFor(runtime) == nullptr ? 0 : RuntimeFor(runtime)->AudioWorkletDeadlineMicrounits();
}

extern "C" int synth_browser_message_tick(synth_browser_runtime* runtime, std::uint64_t timestampMicros)
{
    return RuntimeFor(runtime) == nullptr ? -1 : RuntimeFor(runtime)->MessageTick(timestampMicros);
}

extern "C" const std::uint8_t* synth_browser_build_ui_frame(synth_browser_runtime* runtime, std::size_t* size)
{
    return RuntimeFor(runtime) == nullptr ? nullptr : RuntimeFor(runtime)->BuildUiFrame(size);
}

extern "C" int synth_browser_dispatch_action(synth_browser_runtime* runtime, const char* name, const char* value)
{
    return RuntimeFor(runtime) == nullptr ? -1 : RuntimeFor(runtime)->DispatchAction(name, value);
}

extern "C" int synth_browser_consume_persistence_dirty(synth_browser_runtime* runtime)
{
    return RuntimeFor(runtime) != nullptr && RuntimeFor(runtime)->ConsumePersistenceDirty() ? 1 : 0;
}

extern "C" int synth_browser_submit_midi_endpoints(synth_browser_runtime* runtime,
                                                     const synth_browser::MidiEndpointDescriptor* endpoints,
                                                     std::uint32_t count)
{
    return RuntimeFor(runtime) == nullptr ? -1 : RuntimeFor(runtime)->SubmitMidiEndpoints(endpoints, count);
}

extern "C" int synth_browser_submit_audio_devices(synth_browser_runtime* runtime,
                                                    const synth_browser::AudioDeviceDescriptor* devices,
                                                    std::uint32_t count)
{
    return RuntimeFor(runtime) == nullptr ? -1 : RuntimeFor(runtime)->SubmitAudioDevices(devices, count);
}

extern "C" int synth_browser_dequeue_midi_action(synth_browser_runtime* runtime,
                                                  synth_browser::MidiActionDescriptor* action)
{
    return RuntimeFor(runtime) == nullptr ? -1 : RuntimeFor(runtime)->DequeueMidiAction(action);
}

extern "C" int synth_browser_deliver_midi(synth_browser_runtime* runtime, std::uint32_t controllerIx,
                                            const std::uint8_t* bytes, std::uint32_t size,
                                            std::uint64_t timestampMicros)
{
    return RuntimeFor(runtime) == nullptr
               ? -1
               : RuntimeFor(runtime)->DeliverMidi(controllerIx, bytes, size, timestampMicros);
}

extern "C" const std::uint8_t* synth_browser_dequeue_midi_output(
    synth_browser_runtime* runtime, synth_browser::MidiOutputDescriptor* descriptor)
{
    return RuntimeFor(runtime) == nullptr ? nullptr : RuntimeFor(runtime)->DequeueMidiOutput(descriptor);
}

extern "C" int synth_browser_midi_diagnostics(
    synth_browser_runtime* runtime, synth_browser::MidiDiagnosticsDescriptor* descriptor)
{
    return RuntimeFor(runtime) == nullptr
               ? -1
               : RuntimeFor(runtime)->MidiDiagnostics(descriptor);
}

extern "C" void synth_browser_destroy(synth_browser_runtime* runtime)
{
    if (RuntimeFor(runtime) != nullptr) {
        RuntimeFor(runtime)->Destroy();
    }
}
