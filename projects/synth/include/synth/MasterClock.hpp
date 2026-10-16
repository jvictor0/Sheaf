#pragma once

// JUCE-free musical-clock contracts and the bounded audio-sample/host-time
// mapper. All steady-state operations are fixed-capacity and noexcept.

#include "synth/DspPhasor2Tick.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace synth {

struct SyncConfig {
    bool sendClock = false;
    bool receiveClock = false;
    bool sendTransport = false;
    bool receiveTransport = false;
    int ppqn = 24;

    bool IsValid() const noexcept { return ppqn >= 1 && ppqn <= 960; }
    bool operator==(const SyncConfig&) const = default;
};

enum class ClockTransportState : std::uint8_t {
    Stopped,
    ArmedStart,
    ArmedContinue,
    Running,
};

enum class ClockAcquisitionState : std::uint8_t {
    Internal,
    Acquiring,
    Locked,
    FreeRun,
};

enum class ClockSource : std::uint8_t {
    Internal,
    ExternalMidi,
};

enum class ClockTransportCommand : std::uint8_t {
    Start,
    Continue,
    Stop,
};

enum class ScheduledMidiEventKind : std::uint8_t {
    PhaseGenerationCutoff,
    TimingClock,
    Start,
    Continue,
    Stop,
};

// At equal deadlines a consumer applies generation invalidation first, then
// transport, then clock. Sequence is the deterministic tie-break within one
// ordering class.
enum class ScheduledMidiOrderingIntent : std::uint8_t {
    GenerationCutoff,
    Transport,
    Clock,
};

struct ScheduledMidiEvent {
    ScheduledMidiEventKind kind = ScheduledMidiEventKind::TimingClock;
    ScheduledMidiOrderingIntent orderingIntent = ScheduledMidiOrderingIntent::Clock;
    bool broadcast = true;
    std::uint64_t dueTimeMicros = 0;
    std::uint64_t sequence = 0;
    // Clock events use a nonzero phase generation. Transport events use zero
    // because a grid cutoff must never discard the command which caused it.
    std::uint64_t phaseGeneration = 0;
    // Nonzero only for PhaseGenerationCutoff.
    std::uint64_t invalidatedPhaseGeneration = 0;
    std::uint64_t phaseCutoffDueTimeMicros = 0;

    constexpr std::uint8_t MidiStatusByte() const noexcept {
        switch (kind) {
        case ScheduledMidiEventKind::TimingClock:
            return 0xF8;
        case ScheduledMidiEventKind::Start:
            return 0xFA;
        case ScheduledMidiEventKind::Continue:
            return 0xFB;
        case ScheduledMidiEventKind::Stop:
            return 0xFC;
        case ScheduledMidiEventKind::PhaseGenerationCutoff:
            return 0;
        }
        return 0;
    }
};

// The audio-side producer owns no sink lifetime and never waits. Production
// implementations must provide a fixed-capacity, allocation-free, mutex-free
// newest-drop enqueue; false makes overflow observable to MasterClock.
class IScheduledMidiEventSink {
public:
    virtual ~IScheduledMidiEventSink() = default;
    virtual bool TryEnqueue(const ScheduledMidiEvent& event) noexcept = 0;
};

struct ClockDiagnostics {
    ClockAcquisitionState acquisition = ClockAcquisitionState::Internal;
    ClockSource source = ClockSource::Internal;
    bool hasActiveExternalSource = false;
    std::size_t activeExternalSourceSlot = 0;
    // Filled by MasterClock::DiagnosticsSnapshot from MasterClock::TransportState().
    ClockTransportState transportState = ClockTransportState::Stopped;
    // Manual or filtered-estimator tempo, without the transient PLL phase
    // correction which may be present in QuarterNotesPerSample().
    double currentBpm = 120.0;
    std::uint64_t outputLatencyMicros = 0;
    // Rejected external MIDI clock/transport input only.
    std::uint64_t ignoredInputCount = 0;
    // Rejected audio callback observations are a separate mapper concern.
    std::uint64_t mapperIgnoredObservationCount = 0;
    std::uint64_t lateEventCount = 0;
    std::uint64_t droppedOutputCount = 0;
    std::uint64_t mapperDiscontinuityCount = 0;
    std::uint64_t mapperGeneration = 0;
    bool sourceIsProvisional = false;
    std::uint64_t acceptedExternalClockCount = 0;
    std::uint64_t externalInputTickIndex = 0;
    std::uint64_t inferredMissedPulseCount = 0;
    std::uint64_t hardReacquireCount = 0;
    std::uint64_t sourceTimeoutCount = 0;
    std::uint64_t enumeratedCrossingCount = 0;
    std::uint64_t phaseGeneration = 1;
    double externalRawIntervalMicros = 0.0;
    double externalMedianIntervalMicros = 0.0;
    double externalFilteredPeriodMicros = 0.0;
    double externalPhaseErrorPulsePeriods = 0.0;
    double appliedPhaseCorrectionPulsePeriods = 0.0;
};

struct ClockPlanDescriptor {
    std::uint64_t startSample = 0;
    std::uint64_t endSample = 0;
    double lifetimeStartQuarterNotes = 0.0;
    double transportStartQuarterNotes = 0.0;
    double quarterNotesPerSample = 0.0;
    ClockTransportState transportState = ClockTransportState::Stopped;
    std::uint64_t transportEpoch = 0;
    std::uint64_t generation = 0;

    bool Contains(double absoluteOutputSample) const noexcept;
    double LifetimeQuarterNotesAt(double absoluteOutputSample) const noexcept;
    double TransportQuarterNotesAt(double absoluteOutputSample) const noexcept;
    double LifetimeEndQuarterNotes() const noexcept;
    double TransportEndQuarterNotes() const noexcept;
};

// Applications receive only const access to a plan. Its descriptor contains
// one affine segment and no per-sample storage.
class ClockBlockPlan {
public:
    constexpr ClockBlockPlan() noexcept = default;
    explicit constexpr ClockBlockPlan(ClockPlanDescriptor descriptor) noexcept
        : descriptor_(descriptor) {}

    std::uint64_t StartSample() const noexcept { return descriptor_.startSample; }
    std::uint64_t EndSample() const noexcept { return descriptor_.endSample; }
    std::uint64_t FrameCount() const noexcept {
        return descriptor_.endSample >= descriptor_.startSample
            ? descriptor_.endSample - descriptor_.startSample
            : 0;
    }
    double LifetimeStartQuarterNotes() const noexcept {
        return descriptor_.lifetimeStartQuarterNotes;
    }
    double TransportStartQuarterNotes() const noexcept {
        return descriptor_.transportState == ClockTransportState::Running
            ? descriptor_.transportStartQuarterNotes
            : 0.0;
    }
    double QuarterNotesPerSample() const noexcept {
        return descriptor_.quarterNotesPerSample;
    }
    ClockTransportState TransportState() const noexcept {
        return descriptor_.transportState;
    }
    bool IsTransportRunning() const noexcept {
        return descriptor_.transportState == ClockTransportState::Running;
    }
    std::uint64_t TransportEpoch() const noexcept { return descriptor_.transportEpoch; }
    std::uint64_t Generation() const noexcept { return descriptor_.generation; }
    const ClockPlanDescriptor& Descriptor() const noexcept { return descriptor_; }

    bool Contains(double absoluteOutputSample) const noexcept;
    // Precondition: Contains(absoluteOutputSample). Use the corresponding
    // Try* method when the caller has not already established containment.
    double LifetimeQuarterNotesAt(double absoluteOutputSample) const noexcept;
    // Precondition: Contains(absoluteOutputSample). Use the corresponding
    // Try* method when the caller has not already established containment.
    double TransportQuarterNotesAt(double absoluteOutputSample) const noexcept;
    std::optional<double> TryLifetimeQuarterNotesAt(double absoluteOutputSample) const noexcept;
    std::optional<double> TryTransportQuarterNotesAt(double absoluteOutputSample) const noexcept;
    double LifetimeEndQuarterNotes() const noexcept;
    double TransportEndQuarterNotes() const noexcept;

private:
    ClockPlanDescriptor descriptor_{};
};

enum class MapperObservationResult : std::uint8_t {
    Rejected,
    Anchored,
    Continuous,
    Discontinuity,
};

class AudioSampleTimeMapper {
public:
    static constexpr std::size_t kPhaseErrorWindow = 5;
    static constexpr std::size_t kHistoryCapacity = 64;
    static constexpr double kPhaseErrorEwmaGain = 1.0 / 32.0;
    static constexpr double kMaximumSlewPpm = 500.0;

    struct Diagnostics {
        bool anchored = false;
        std::uint64_t generation = 0;
        std::uint64_t discontinuityCount = 0;
        std::uint64_t lateEventCount = 0;
        std::uint64_t ignoredObservationCount = 0;
        std::size_t phaseErrorCount = 0;
        double latestPhaseErrorMicros = 0.0;
        double medianPhaseErrorMicros = 0.0;
        double filteredPhaseErrorMicros = 0.0;
        double currentMicrosPerSample = 0.0;
    };

    bool Prepare(double sampleRate, std::uint64_t outputLookaheadMicros) noexcept;
    bool IsPrepared() const noexcept { return prepared_; }
    double SampleRate() const noexcept { return sampleRate_; }
    double NominalMicrosPerSample() const noexcept { return nominalMicrosPerSample_; }
    std::uint64_t OutputLookaheadMicros() const noexcept { return outputLookaheadMicros_; }

    MapperObservationResult ObserveBlock(
        std::uint64_t blockStartSample,
        std::uint64_t callbackTimestampMicros) noexcept;

    std::optional<double> TimeMicrosAt(double absoluteOutputSample) const noexcept;
    std::optional<std::uint64_t> TimestampMicrosAt(double absoluteOutputSample) const noexcept;
    std::optional<double> SampleAtTimestamp(std::uint64_t timestampMicros) const noexcept;
    Diagnostics DiagnosticsSnapshot() const noexcept { return diagnostics_; }
    std::size_t HistorySize() const noexcept { return segmentCount_; }

private:
    struct Segment {
        double startSample = 0.0;
        double startTimeMicros = 0.0;
        double microsPerSample = 0.0;
        std::uint64_t generation = 0;
    };

    const Segment* SegmentAtSample(double absoluteOutputSample) const noexcept;
    const Segment* SegmentAtTimestamp(double timestampMicros, double& endSample) const noexcept;
    const Segment& SegmentAtLogicalIndex(std::size_t logicalIndex) const noexcept;
    void PushSegment(const Segment& segment) noexcept;
    void PushPhaseError(double errorMicros) noexcept;
    double MedianPhaseError() const noexcept;
    void ClearPhaseErrors() noexcept;

    std::array<Segment, kHistoryCapacity> segments_{};
    std::size_t segmentHead_ = 0;
    std::size_t segmentCount_ = 0;
    std::array<double, kPhaseErrorWindow> phaseErrors_{};
    std::size_t phaseErrorHead_ = 0;
    std::size_t phaseErrorCount_ = 0;
    double sampleRate_ = 0.0;
    double nominalMicrosPerSample_ = 0.0;
    double filteredPhaseErrorMicros_ = 0.0;
    std::uint64_t outputLookaheadMicros_ = 0;
    std::uint64_t lastObservedSample_ = 0;
    bool prepared_ = false;
    bool anchored_ = false;
    Diagnostics diagnostics_{};
};

struct ClockTimePoint {
    double samplePosition = 0.0;
    double lifetimeQuarterNotes = 0.0;
    double transportQuarterNotes = 0.0;
    ClockTransportState transportState = ClockTransportState::Stopped;
    std::uint64_t transportEpoch = 0;
};

class MasterClock {
public:
    static constexpr std::size_t kPlanHistoryCapacity = 64;
    static constexpr std::size_t kExternalIntervalWindow = 5;
    static constexpr std::size_t kPendingInternalTransportCapacity = 16;
    static constexpr std::size_t kMaximumCrossingsPerSegment = 4096;
    static constexpr std::size_t kMaximumSpliceCrossingIterations = 4096;
    static constexpr double kDefaultTempoBpm = 120.0;
    static constexpr double kExternalPeriodEwmaGain = 1.0 / 8.0;
    static constexpr double kMissedPulseTolerance = 0.25;
    static constexpr double kPhaseCorrectionGain = 0.25;
    static constexpr double kMaximumPhaseCorrectionPulsePeriods = 0.25;
    static constexpr double kHardReacquirePulsePeriods = 2.0;
    static constexpr std::uint64_t kMinimumSourceTimeoutMicros = 500'000;

    bool Prepare(double sampleRate, std::size_t blockSize) noexcept;
    bool IsPrepared() const noexcept { return prepared_; }
    double SampleRate() const noexcept { return sampleRate_; }
    std::size_t BlockSize() const noexcept { return blockSize_; }
    std::uint64_t OutputLatencyMicros() const noexcept { return outputLatencyMicros_; }
    void SetOutputSchedulingHorizonMicros(std::uint64_t horizonMicros) noexcept {
        outputSchedulingHorizonMicros_ = horizonMicros;
    }
    std::uint64_t OutputSchedulingHorizonMicros() const noexcept {
        return outputSchedulingHorizonMicros_;
    }

    bool SetTempoBpm(double bpm) noexcept;
    // Authority tempo is deliberately uncorrected. During external recovery,
    // QuarterNotesPerSample() may additionally contain bounded phase correction.
    double TempoBpm() const noexcept { return activeBpm_; }
    double QuarterNotesPerSample() const noexcept { return pendingQuarterNotesPerSample_; }
    double LifetimeQuarterNotes() const noexcept;
    double TransportQuarterNotes() const noexcept;
    ClockTransportState TransportState() const noexcept {
        return hasCurrentPlan_ ? currentPlan_.TransportState() : transportState_;
    }
    bool IsTransportRunning() const noexcept {
        return TransportState() == ClockTransportState::Running;
    }

    bool ApplySyncConfig(const SyncConfig& config) noexcept;
    const SyncConfig& SyncConfiguration() const noexcept { return syncConfig_; }
    ClockDiagnostics DiagnosticsSnapshot() const noexcept;

    void SetScheduledMidiEventSink(IScheduledMidiEventSink* sink) noexcept {
        scheduledEventSink_ = sink;
    }
    IScheduledMidiEventSink* ScheduledMidiEventSink() const noexcept {
        return scheduledEventSink_;
    }

    bool HandleInternalTransport(ClockTransportCommand command) noexcept;
    bool HandleExternalTransport(
        ClockTransportCommand command,
        std::uint64_t timestampMicros,
        std::size_t controllerSlot) noexcept;
    bool HandleExternalClock(
        std::uint64_t timestampMicros,
        std::size_t controllerSlot) noexcept;

    const ClockBlockPlan* CommitBlock(
        std::uint64_t blockStartSample,
        std::size_t frameCount,
        std::uint64_t callbackTimestampMicros) noexcept;
    const ClockBlockPlan* CurrentPlan() const noexcept {
        return hasCurrentPlan_ ? &currentPlan_ : nullptr;
    }

    std::size_t PlanHistorySize() const noexcept { return planHistoryCount_; }
    std::optional<ClockBlockPlan> PlanAtSample(double absoluteOutputSample) const noexcept;
    std::optional<ClockTimePoint> TimeAtSample(double absoluteOutputSample) const noexcept;
    std::optional<ClockTimePoint> TimeAtTimestamp(std::uint64_t timestampMicros) const noexcept;
    std::optional<double> SampleAtTimestamp(std::uint64_t timestampMicros) const noexcept {
        return timeMapper_.SampleAtTimestamp(timestampMicros);
    }
    const AudioSampleTimeMapper& TimeMapper() const noexcept { return timeMapper_; }

private:
    struct ExternalEstimatorState {
        std::array<double, kExternalIntervalWindow> normalizedIntervals{};
        std::size_t intervalHead = 0;
        std::size_t intervalCount = 0;
        std::uint64_t lastClockTimestampMicros = 0;
        std::uint64_t inputTickIndex = 0;
        std::uint64_t validIntervalCount = 0;
        std::uint64_t acceptedClockCount = 0;
        std::uint64_t inferredMissedPulseCount = 0;
        std::uint64_t hardReacquireCount = 0;
        double rawIntervalMicros = 0.0;
        double medianIntervalMicros = 0.0;
        double filteredPeriodMicros = 0.0;
        double phaseAnchorQuarterNotes = 0.0;
        double phaseErrorPulsePeriods = 0.0;
        double appliedPhaseCorrectionPulsePeriods = 0.0;
        bool hasLastClock = false;
        bool hasPeriodEstimate = false;
        bool hasPhaseAnchor = false;
    };

    const ClockPlanDescriptor& PlanAtLogicalIndex(std::size_t logicalIndex) const noexcept;
    void PushPlan(const ClockPlanDescriptor& descriptor) noexcept;
    void ResetExternalEstimatorForSource() noexcept;
    void ClearExternalSource(bool timedOut) noexcept;
    void ExpireExternalSource(std::uint64_t nowMicros) noexcept;
    std::uint64_t ExternalSourceTimeoutMicros() const noexcept;
    bool AcceptExternalSource(
        std::size_t controllerSlot,
        std::uint64_t timestampMicros,
        bool mayClaim,
        bool provisionalClaim) noexcept;
    void PushExternalInterval(double intervalMicros) noexcept;
    double MedianExternalInterval() const noexcept;
    void ApplyExternalPhaseObservation(std::uint64_t timestampMicros) noexcept;
    void ApplyPendingInternalTransport(std::uint64_t boundaryTimestampMicros) noexcept;
    void ApplyOutputConfigurationAtBoundary(std::uint64_t boundaryTimestampMicros) noexcept;
    void ActivateTransportAtExternalTimestamp(
        std::uint64_t timestampMicros,
        std::optional<ClockTransportCommand> transportAtSameTimestamp = std::nullopt) noexcept;
    void StopTransportAtExternalTimestamp(std::uint64_t timestampMicros) noexcept;
    std::optional<double> LifetimeQuarterNotesAtKnownSample(double sample) const noexcept;
    void PrimeOutputDetector(double phaseQuarterNotes) noexcept;
    void BeginPhaseGeneration(std::uint64_t cutoffTimestampMicros) noexcept;
    void EnumerateCurrentPlanCrossings() noexcept;
    void EnumerateSpliceCrossings(
        double spliceSample,
        double phaseAtSplice,
        bool useLifetimePhase) noexcept;
    void EnumerateAffineCrossings(
        double startSample,
        double endSample,
        double phaseAtStart,
        double quarterNotesPerSample,
        std::size_t& remainingCandidateIterations) noexcept;
    std::optional<std::uint64_t> DueTimeAtSample(double sample) const noexcept;
    std::optional<std::uint64_t> DueTimeFromTimestamp(std::uint64_t timestampMicros) const noexcept;
    bool EnqueueScheduledEvent(
        ScheduledMidiEventKind kind,
        std::uint64_t dueTimeMicros,
        std::uint64_t phaseGeneration = 0,
        std::uint64_t invalidatedGeneration = 0) noexcept;
    void EmitTransport(ClockTransportCommand command, std::uint64_t dueTimeMicros) noexcept;
    void EmitExplicitZeroClock(std::uint64_t dueTimeMicros) noexcept;
    double SelectedPhaseAtNextBoundary() const noexcept;

    std::array<ClockPlanDescriptor, kPlanHistoryCapacity> planHistory_{};
    std::size_t planHistoryHead_ = 0;
    std::size_t planHistoryCount_ = 0;
    ClockBlockPlan currentPlan_{};
    AudioSampleTimeMapper timeMapper_{};
    Phasor2Tick outputTickDetector_{};
    IScheduledMidiEventSink* scheduledEventSink_ = nullptr;
    SyncConfig syncConfig_{};
    ClockTransportState transportState_ = ClockTransportState::Stopped;
    ClockAcquisitionState acquisitionState_ = ClockAcquisitionState::Internal;
    ClockSource source_ = ClockSource::Internal;
    double sampleRate_ = 0.0;
    double manualBpm_ = kDefaultTempoBpm;
    // Manual or filtered external-estimator tempo; never the phase-corrected slope.
    double activeBpm_ = kDefaultTempoBpm;
    double pendingQuarterNotesPerSample_ = 0.0;
    double nextLifetimeQuarterNotes_ = 0.0;
    double nextTransportQuarterNotes_ = 0.0;
    std::size_t blockSize_ = 0;
    std::uint64_t outputSchedulingHorizonMicros_ = 0;
    std::uint64_t outputLatencyMicros_ = 0;
    std::uint64_t expectedNextSample_ = 0;
    std::uint64_t transportEpoch_ = 0;
    std::uint64_t planGeneration_ = 1;
    std::uint64_t ignoredInputCount_ = 0;
    std::uint64_t droppedOutputCount_ = 0;
    std::uint64_t scheduledSequence_ = 0;
    std::uint64_t phaseGeneration_ = 1;
    std::uint64_t enumeratedCrossingCount_ = 0;
    std::uint64_t sourceTimeoutCount_ = 0;
    std::array<ClockTransportCommand, kPendingInternalTransportCapacity>
        pendingInternalTransportOutputs_{};
    std::size_t pendingInternalTransportOutputCount_ = 0;
    ClockTransportCommand pendingInternalTransport_ = ClockTransportCommand::Stop;
    ExternalEstimatorState externalEstimator_{};
    std::size_t activeExternalSourceSlot_ = 0;
    std::uint64_t activeSourceLastActivityMicros_ = 0;
    bool hasPendingInternalTransport_ = false;
    bool hasActiveExternalSource_ = false;
    bool sourceIsProvisional_ = false;
    bool outputConfigurationPending_ = false;
    int detectorPpqn_ = 24;
    bool prepared_ = false;
    bool hasCurrentPlan_ = false;
};

} // namespace synth
