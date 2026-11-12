#include "synth/MasterClock.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace synth {

namespace {

bool IsFinitePositive(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

std::optional<std::uint64_t> AddMicros(
    std::uint64_t timestampMicros,
    std::uint64_t offsetMicros) noexcept {
    if (offsetMicros > std::numeric_limits<std::uint64_t>::max() - timestampMicros) {
        return std::nullopt;
    }
    return timestampMicros + offsetMicros;
}

ScheduledMidiEventKind EventKindForCommand(ClockTransportCommand command) noexcept {
    switch (command) {
    case ClockTransportCommand::Start:
        return ScheduledMidiEventKind::Start;
    case ClockTransportCommand::Continue:
        return ScheduledMidiEventKind::Continue;
    case ClockTransportCommand::Stop:
        return ScheduledMidiEventKind::Stop;
    }
    return ScheduledMidiEventKind::Stop;
}

bool IsStartCommand(ClockTransportCommand command) noexcept {
    return command == ClockTransportCommand::Start ||
           command == ClockTransportCommand::Continue;
}

} // namespace

bool ClockPlanDescriptor::Contains(double absoluteOutputSample) const noexcept {
    return std::isfinite(absoluteOutputSample) &&
           absoluteOutputSample >= static_cast<double>(startSample) &&
           absoluteOutputSample < static_cast<double>(endSample);
}

double ClockPlanDescriptor::LifetimeQuarterNotesAt(double absoluteOutputSample) const noexcept {
    return lifetimeStartQuarterNotes +
           (absoluteOutputSample - static_cast<double>(startSample)) * quarterNotesPerSample;
}

double ClockPlanDescriptor::TransportQuarterNotesAt(double absoluteOutputSample) const noexcept {
    if (transportState != ClockTransportState::Running) {
        return 0.0;
    }
    return transportStartQuarterNotes +
           (absoluteOutputSample - static_cast<double>(startSample)) * quarterNotesPerSample;
}

double ClockPlanDescriptor::LifetimeEndQuarterNotes() const noexcept {
    return LifetimeQuarterNotesAt(static_cast<double>(endSample));
}

double ClockPlanDescriptor::TransportEndQuarterNotes() const noexcept {
    if (transportState != ClockTransportState::Running) {
        return 0.0;
    }
    return TransportQuarterNotesAt(static_cast<double>(endSample));
}

bool ClockBlockPlan::Contains(double absoluteOutputSample) const noexcept {
    return descriptor_.Contains(absoluteOutputSample);
}

double ClockBlockPlan::LifetimeQuarterNotesAt(double absoluteOutputSample) const noexcept {
    assert(Contains(absoluteOutputSample));
    return descriptor_.LifetimeQuarterNotesAt(absoluteOutputSample);
}

double ClockBlockPlan::TransportQuarterNotesAt(double absoluteOutputSample) const noexcept {
    assert(Contains(absoluteOutputSample));
    return descriptor_.TransportQuarterNotesAt(absoluteOutputSample);
}

std::optional<double> ClockBlockPlan::TryLifetimeQuarterNotesAt(
    double absoluteOutputSample) const noexcept {
    if (!Contains(absoluteOutputSample)) {
        return std::nullopt;
    }
    return descriptor_.LifetimeQuarterNotesAt(absoluteOutputSample);
}

std::optional<double> ClockBlockPlan::TryTransportQuarterNotesAt(
    double absoluteOutputSample) const noexcept {
    if (!Contains(absoluteOutputSample)) {
        return std::nullopt;
    }
    return descriptor_.TransportQuarterNotesAt(absoluteOutputSample);
}

double ClockBlockPlan::LifetimeEndQuarterNotes() const noexcept {
    return descriptor_.LifetimeEndQuarterNotes();
}

double ClockBlockPlan::TransportEndQuarterNotes() const noexcept {
    return descriptor_.TransportEndQuarterNotes();
}

bool AudioSampleTimeMapper::Prepare(
    double sampleRate,
    std::uint64_t outputLookaheadMicros) noexcept {
    if (!IsFinitePositive(sampleRate) || outputLookaheadMicros == 0) {
        return false;
    }

    sampleRate_ = sampleRate;
    nominalMicrosPerSample_ = 1'000'000.0 / sampleRate;
    outputLookaheadMicros_ = outputLookaheadMicros;
    segmentHead_ = 0;
    segmentCount_ = 0;
    lastObservedSample_ = 0;
    prepared_ = true;
    anchored_ = false;
    ClearPhaseErrors();
    diagnostics_ = {};
    diagnostics_.currentMicrosPerSample = nominalMicrosPerSample_;
    return true;
}

MapperObservationResult AudioSampleTimeMapper::ObserveBlock(
    std::uint64_t blockStartSample,
    std::uint64_t callbackTimestampMicros) noexcept {
    if (!prepared_ || (anchored_ && blockStartSample <= lastObservedSample_)) {
        ++diagnostics_.ignoredObservationCount;
        return MapperObservationResult::Rejected;
    }

    if (!anchored_) {
        diagnostics_.generation = 1;
        PushSegment(Segment{
            .startSample = static_cast<double>(blockStartSample),
            .startTimeMicros = static_cast<double>(callbackTimestampMicros),
            .microsPerSample = nominalMicrosPerSample_,
            .generation = diagnostics_.generation,
        });
        lastObservedSample_ = blockStartSample;
        anchored_ = true;
        diagnostics_.anchored = true;
        diagnostics_.currentMicrosPerSample = nominalMicrosPerSample_;
        return MapperObservationResult::Anchored;
    }

    const auto predictedValue = TimeMicrosAt(static_cast<double>(blockStartSample));
    if (!predictedValue.has_value()) {
        ++diagnostics_.ignoredObservationCount;
        return MapperObservationResult::Rejected;
    }
    const double predicted = *predictedValue;
    const double observed = static_cast<double>(callbackTimestampMicros);
    const double error = observed - predicted;
    diagnostics_.latestPhaseErrorMicros = error;

    if (std::fabs(error) > static_cast<double>(outputLookaheadMicros_)) {
        ClearPhaseErrors();
        diagnostics_.latestPhaseErrorMicros = error;
        ++diagnostics_.generation;
        ++diagnostics_.discontinuityCount;
        ++diagnostics_.lateEventCount;
        const double resetTime = std::max(observed, predicted);
        PushSegment(Segment{
            .startSample = static_cast<double>(blockStartSample),
            .startTimeMicros = resetTime,
            .microsPerSample = nominalMicrosPerSample_,
            .generation = diagnostics_.generation,
        });
        diagnostics_.currentMicrosPerSample = nominalMicrosPerSample_;
        lastObservedSample_ = blockStartSample;
        return MapperObservationResult::Discontinuity;
    }

    PushPhaseError(error);
    const double median = MedianPhaseError();
    filteredPhaseErrorMicros_ +=
        (median - filteredPhaseErrorMicros_) * kPhaseErrorEwmaGain;
    const double sampleDelta = static_cast<double>(blockStartSample - lastObservedSample_);
    const double maximumAdjustment = nominalMicrosPerSample_ * kMaximumSlewPpm / 1'000'000.0;
    const double desiredAdjustment = filteredPhaseErrorMicros_ / sampleDelta;
    const double nextSlope = nominalMicrosPerSample_ +
        std::clamp(desiredAdjustment, -maximumAdjustment, maximumAdjustment);

    PushSegment(Segment{
        .startSample = static_cast<double>(blockStartSample),
        .startTimeMicros = predicted,
        .microsPerSample = nextSlope,
        .generation = diagnostics_.generation,
    });
    diagnostics_.medianPhaseErrorMicros = median;
    diagnostics_.filteredPhaseErrorMicros = filteredPhaseErrorMicros_;
    diagnostics_.currentMicrosPerSample = nextSlope;
    diagnostics_.phaseErrorCount = phaseErrorCount_;
    lastObservedSample_ = blockStartSample;
    return MapperObservationResult::Continuous;
}

std::optional<double> AudioSampleTimeMapper::TimeMicrosAt(
    double absoluteOutputSample) const noexcept {
    if (!prepared_ || !anchored_ || !std::isfinite(absoluteOutputSample)) {
        return std::nullopt;
    }
    const Segment* segment = SegmentAtSample(absoluteOutputSample);
    if (segment == nullptr) {
        return std::nullopt;
    }
    const double value = segment->startTimeMicros +
        (absoluteOutputSample - segment->startSample) * segment->microsPerSample;
    if (!std::isfinite(value) || value < 0.0) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint64_t> AudioSampleTimeMapper::TimestampMicrosAt(
    double absoluteOutputSample) const noexcept {
    const auto value = TimeMicrosAt(absoluteOutputSample);
    if (!value.has_value()) {
        return std::nullopt;
    }
    const long double rounded = std::floor(static_cast<long double>(*value) + 0.5L);
    if (rounded < 0.0L ||
        rounded > static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(rounded);
}

std::optional<double> AudioSampleTimeMapper::SampleAtTimestamp(
    std::uint64_t timestampMicros) const noexcept {
    if (!prepared_ || !anchored_) {
        return std::nullopt;
    }
    double endSample = 0.0;
    const double timestamp = static_cast<double>(timestampMicros);
    const Segment* segment = SegmentAtTimestamp(timestamp, endSample);
    if (segment == nullptr) {
        return std::nullopt;
    }
    const double sample = segment->startSample +
        (timestamp - segment->startTimeMicros) / segment->microsPerSample;
    if (!std::isfinite(sample) || sample < segment->startSample || sample >= endSample) {
        return std::nullopt;
    }
    return sample;
}

const AudioSampleTimeMapper::Segment* AudioSampleTimeMapper::SegmentAtSample(
    double absoluteOutputSample) const noexcept {
    for (std::size_t logical = segmentCount_; logical > 0; --logical) {
        const Segment& segment = SegmentAtLogicalIndex(logical - 1);
        if (absoluteOutputSample >= segment.startSample) {
            return &segment;
        }
    }
    return nullptr;
}

const AudioSampleTimeMapper::Segment* AudioSampleTimeMapper::SegmentAtTimestamp(
    double timestampMicros,
    double& endSample) const noexcept {
    for (std::size_t logical = segmentCount_; logical > 0; --logical) {
        const std::size_t index = logical - 1;
        const Segment& segment = SegmentAtLogicalIndex(index);
        if (timestampMicros < segment.startTimeMicros) {
            continue;
        }
        endSample = index + 1 < segmentCount_
            ? SegmentAtLogicalIndex(index + 1).startSample
            : std::numeric_limits<double>::infinity();
        const double sample = segment.startSample +
            (timestampMicros - segment.startTimeMicros) / segment.microsPerSample;
        if (sample < endSample) {
            return &segment;
        }
    }
    return nullptr;
}

const AudioSampleTimeMapper::Segment& AudioSampleTimeMapper::SegmentAtLogicalIndex(
    std::size_t logicalIndex) const noexcept {
    return segments_[(segmentHead_ + logicalIndex) % kHistoryCapacity];
}

void AudioSampleTimeMapper::PushSegment(const Segment& segment) noexcept {
    if (segmentCount_ < kHistoryCapacity) {
        segments_[(segmentHead_ + segmentCount_) % kHistoryCapacity] = segment;
        ++segmentCount_;
        return;
    }
    segments_[segmentHead_] = segment;
    segmentHead_ = (segmentHead_ + 1) % kHistoryCapacity;
}

void AudioSampleTimeMapper::PushPhaseError(double errorMicros) noexcept {
    if (phaseErrorCount_ < kPhaseErrorWindow) {
        phaseErrors_[(phaseErrorHead_ + phaseErrorCount_) % kPhaseErrorWindow] = errorMicros;
        ++phaseErrorCount_;
        return;
    }
    phaseErrors_[phaseErrorHead_] = errorMicros;
    phaseErrorHead_ = (phaseErrorHead_ + 1) % kPhaseErrorWindow;
}

double AudioSampleTimeMapper::MedianPhaseError() const noexcept {
    std::array<double, kPhaseErrorWindow> sorted{};
    for (std::size_t index = 0; index < phaseErrorCount_; ++index) {
        sorted[index] = phaseErrors_[(phaseErrorHead_ + index) % kPhaseErrorWindow];
    }
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(phaseErrorCount_));
    if ((phaseErrorCount_ % 2) != 0) {
        return sorted[phaseErrorCount_ / 2];
    }
    const std::size_t upper = phaseErrorCount_ / 2;
    return (sorted[upper - 1] + sorted[upper]) * 0.5;
}

void AudioSampleTimeMapper::ClearPhaseErrors() noexcept {
    phaseErrors_.fill(0.0);
    phaseErrorHead_ = 0;
    phaseErrorCount_ = 0;
    filteredPhaseErrorMicros_ = 0.0;
    diagnostics_.phaseErrorCount = 0;
    diagnostics_.latestPhaseErrorMicros = 0.0;
    diagnostics_.medianPhaseErrorMicros = 0.0;
    diagnostics_.filteredPhaseErrorMicros = 0.0;
}

void MasterClock::ResetExternalEstimatorForSource() noexcept {
    const std::uint64_t acceptedClockCount = externalEstimator_.acceptedClockCount;
    const std::uint64_t inferredMissedPulseCount =
        externalEstimator_.inferredMissedPulseCount;
    const std::uint64_t hardReacquireCount = externalEstimator_.hardReacquireCount;
    externalEstimator_ = {};
    externalEstimator_.acceptedClockCount = acceptedClockCount;
    externalEstimator_.inferredMissedPulseCount = inferredMissedPulseCount;
    externalEstimator_.hardReacquireCount = hardReacquireCount;
}

void MasterClock::ClearExternalSource(bool timedOut) noexcept {
    hasActiveExternalSource_ = false;
    sourceIsProvisional_ = false;
    activeSourceLastActivityMicros_ = 0;
    ResetExternalEstimatorForSource();
    if (!syncConfig_.receiveClock) {
        acquisitionState_ = ClockAcquisitionState::Internal;
        source_ = ClockSource::Internal;
        return;
    }
    source_ = ClockSource::ExternalMidi;
    acquisitionState_ = timedOut
        ? ClockAcquisitionState::FreeRun
        : ClockAcquisitionState::Acquiring;
    if (timedOut) {
        ++sourceTimeoutCount_;
    }
}

std::uint64_t MasterClock::ExternalSourceTimeoutMicros() const noexcept {
    double periodMicros = externalEstimator_.hasPeriodEstimate
        ? externalEstimator_.filteredPeriodMicros
        : 60'000'000.0 / (static_cast<double>(syncConfig_.ppqn) * activeBpm_);
    if (!IsFinitePositive(periodMicros)) {
        return kMinimumSourceTimeoutMicros;
    }
    const double fourPeriods = 4.0 * periodMicros;
    if (!std::isfinite(fourPeriods) ||
        fourPeriods >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return std::max(
        kMinimumSourceTimeoutMicros,
        static_cast<std::uint64_t>(std::ceil(fourPeriods)));
}

void MasterClock::ExpireExternalSource(std::uint64_t nowMicros) noexcept {
    if (!hasActiveExternalSource_ || nowMicros <= activeSourceLastActivityMicros_) {
        return;
    }
    if (nowMicros - activeSourceLastActivityMicros_ > ExternalSourceTimeoutMicros()) {
        ClearExternalSource(true);
    }
}

bool MasterClock::AcceptExternalSource(
    std::size_t controllerSlot,
    std::uint64_t timestampMicros,
    bool mayClaim,
    bool provisionalClaim) noexcept {
    ExpireExternalSource(timestampMicros);
    if (hasActiveExternalSource_) {
        if (controllerSlot != activeExternalSourceSlot_) {
            ++ignoredInputCount_;
            return false;
        }
        return true;
    }
    if (!mayClaim) {
        return true;
    }

    hasActiveExternalSource_ = true;
    activeExternalSourceSlot_ = controllerSlot;
    activeSourceLastActivityMicros_ = timestampMicros;
    sourceIsProvisional_ = provisionalClaim;
    ResetExternalEstimatorForSource();
    if (syncConfig_.receiveClock) {
        acquisitionState_ = ClockAcquisitionState::Acquiring;
        source_ = ClockSource::ExternalMidi;
    } else {
        acquisitionState_ = ClockAcquisitionState::Internal;
        source_ = ClockSource::Internal;
    }
    return true;
}

void MasterClock::PushExternalInterval(double intervalMicros) noexcept {
    if (externalEstimator_.intervalCount < kExternalIntervalWindow) {
        externalEstimator_.normalizedIntervals[
            (externalEstimator_.intervalHead + externalEstimator_.intervalCount) %
            kExternalIntervalWindow] = intervalMicros;
        ++externalEstimator_.intervalCount;
        return;
    }
    externalEstimator_.normalizedIntervals[externalEstimator_.intervalHead] = intervalMicros;
    externalEstimator_.intervalHead =
        (externalEstimator_.intervalHead + 1) % kExternalIntervalWindow;
}

double MasterClock::MedianExternalInterval() const noexcept {
    std::array<double, kExternalIntervalWindow> sorted{};
    for (std::size_t index = 0; index < externalEstimator_.intervalCount; ++index) {
        sorted[index] = externalEstimator_.normalizedIntervals[
            (externalEstimator_.intervalHead + index) % kExternalIntervalWindow];
    }
    std::sort(
        sorted.begin(),
        sorted.begin() + static_cast<std::ptrdiff_t>(externalEstimator_.intervalCount));
    if ((externalEstimator_.intervalCount % 2) != 0) {
        return sorted[externalEstimator_.intervalCount / 2];
    }
    const std::size_t upper = externalEstimator_.intervalCount / 2;
    return (sorted[upper - 1] + sorted[upper]) * 0.5;
}

void MasterClock::ApplyExternalPhaseObservation(std::uint64_t timestampMicros) noexcept {
    if (!externalEstimator_.hasPeriodEstimate || !prepared_) {
        return;
    }

    const double baseQuarterNotesPerSample =
        1'000'000.0 /
        (static_cast<double>(syncConfig_.ppqn) *
         externalEstimator_.filteredPeriodMicros * sampleRate_);
    if (!IsFinitePositive(baseQuarterNotesPerSample)) {
        ++ignoredInputCount_;
        return;
    }

    const auto point = TimeAtTimestamp(timestampMicros);
    if (!point.has_value()) {
        pendingQuarterNotesPerSample_ = baseQuarterNotesPerSample;
        externalEstimator_.phaseErrorPulsePeriods = 0.0;
        externalEstimator_.appliedPhaseCorrectionPulsePeriods = 0.0;
        return;
    }

    if (!externalEstimator_.hasPhaseAnchor) {
        externalEstimator_.phaseAnchorQuarterNotes =
            point->lifetimeQuarterNotes -
            static_cast<double>(externalEstimator_.inputTickIndex) /
                static_cast<double>(syncConfig_.ppqn);
        externalEstimator_.hasPhaseAnchor = true;
        pendingQuarterNotesPerSample_ = baseQuarterNotesPerSample;
        return;
    }

    const double targetQuarterNotes =
        externalEstimator_.phaseAnchorQuarterNotes +
        static_cast<double>(externalEstimator_.inputTickIndex) /
            static_cast<double>(syncConfig_.ppqn);
    const double phaseErrorPulsePeriods =
        (targetQuarterNotes - point->lifetimeQuarterNotes) *
        static_cast<double>(syncConfig_.ppqn);
    externalEstimator_.phaseErrorPulsePeriods = phaseErrorPulsePeriods;

    if (std::fabs(phaseErrorPulsePeriods) > kHardReacquirePulsePeriods) {
        externalEstimator_.phaseAnchorQuarterNotes =
            point->lifetimeQuarterNotes -
            static_cast<double>(externalEstimator_.inputTickIndex) /
                static_cast<double>(syncConfig_.ppqn);
        externalEstimator_.phaseErrorPulsePeriods = 0.0;
        externalEstimator_.appliedPhaseCorrectionPulsePeriods = 0.0;
        ++externalEstimator_.hardReacquireCount;
        pendingQuarterNotesPerSample_ = baseQuarterNotesPerSample;
        return;
    }

    const double correctionPulsePeriods = std::clamp(
        phaseErrorPulsePeriods * kPhaseCorrectionGain,
        -kMaximumPhaseCorrectionPulsePeriods,
        kMaximumPhaseCorrectionPulsePeriods);
    externalEstimator_.appliedPhaseCorrectionPulsePeriods = correctionPulsePeriods;
    const double corrected =
        baseQuarterNotesPerSample * (1.0 + correctionPulsePeriods);
    pendingQuarterNotesPerSample_ = IsFinitePositive(corrected)
        ? corrected
        : baseQuarterNotesPerSample;
}

std::optional<std::uint64_t> MasterClock::DueTimeFromTimestamp(
    std::uint64_t timestampMicros) const noexcept {
    return AddMicros(timestampMicros, outputLatencyMicros_);
}

std::optional<std::uint64_t> MasterClock::DueTimeAtSample(double sample) const noexcept {
    const auto timestamp = timeMapper_.TimestampMicrosAt(sample);
    if (!timestamp.has_value()) {
        return std::nullopt;
    }
    return DueTimeFromTimestamp(*timestamp);
}

bool MasterClock::EnqueueScheduledEvent(
    ScheduledMidiEventKind kind,
    std::uint64_t dueTimeMicros,
    std::uint64_t phaseGeneration,
    std::uint64_t invalidatedGeneration) noexcept {
    ScheduledMidiOrderingIntent ordering = ScheduledMidiOrderingIntent::Transport;
    if (kind == ScheduledMidiEventKind::PhaseGenerationCutoff) {
        ordering = ScheduledMidiOrderingIntent::GenerationCutoff;
    } else if (kind == ScheduledMidiEventKind::TimingClock) {
        ordering = ScheduledMidiOrderingIntent::Clock;
    }
    const ScheduledMidiEvent event{
        .kind = kind,
        .orderingIntent = ordering,
        .broadcast = true,
        .dueTimeMicros = dueTimeMicros,
        .sequence = ++scheduledSequence_,
        .phaseGeneration = phaseGeneration,
        .invalidatedPhaseGeneration = invalidatedGeneration,
        .phaseCutoffDueTimeMicros =
            kind == ScheduledMidiEventKind::PhaseGenerationCutoff ? dueTimeMicros : 0,
    };
    if (scheduledEventSink_ == nullptr || scheduledEventSink_->TryEnqueue(event)) {
        return true;
    }
    ++droppedOutputCount_;
    return false;
}

void MasterClock::EmitTransport(
    ClockTransportCommand command,
    std::uint64_t dueTimeMicros) noexcept {
    if (syncConfig_.sendTransport) {
        EnqueueScheduledEvent(EventKindForCommand(command), dueTimeMicros);
    }
}

void MasterClock::EmitExplicitZeroClock(std::uint64_t dueTimeMicros) noexcept {
    if (syncConfig_.sendClock) {
        EnqueueScheduledEvent(
            ScheduledMidiEventKind::TimingClock,
            dueTimeMicros,
            phaseGeneration_);
        ++enumeratedCrossingCount_;
    }
}

void MasterClock::BeginPhaseGeneration(std::uint64_t cutoffTimestampMicros) noexcept {
    const std::uint64_t invalidatedGeneration = phaseGeneration_;
    ++phaseGeneration_;
    planGeneration_ = phaseGeneration_;
    const auto dueTime = DueTimeFromTimestamp(cutoffTimestampMicros);
    if (!dueTime.has_value()) {
        ++droppedOutputCount_;
        return;
    }
    EnqueueScheduledEvent(
        ScheduledMidiEventKind::PhaseGenerationCutoff,
        *dueTime,
        phaseGeneration_,
        invalidatedGeneration);
}

void MasterClock::PrimeOutputDetector(double phaseQuarterNotes) noexcept {
    if (!outputTickDetector_.Prime(Phasor2Tick::Input{
            .time = phaseQuarterNotes,
            .multiplier = detectorPpqn_,
        })) {
        ++droppedOutputCount_;
    }
}

double MasterClock::SelectedPhaseAtNextBoundary() const noexcept {
    return transportState_ == ClockTransportState::Running
        ? nextTransportQuarterNotes_
        : nextLifetimeQuarterNotes_;
}

void MasterClock::EnumerateAffineCrossings(
    double startSample,
    double endSample,
    double phaseAtStart,
    double quarterNotesPerSample,
    std::size_t& remainingCandidateIterations) noexcept {
    if (!syncConfig_.sendClock || !(endSample > startSample) ||
        !std::isfinite(phaseAtStart) || !IsFinitePositive(quarterNotesPerSample) ||
        detectorPpqn_ <= 0) {
        return;
    }
    const double phaseAtEnd =
        phaseAtStart + (endSample - startSample) * quarterNotesPerSample;
    const long double scaledStart =
        static_cast<long double>(phaseAtStart) * detectorPpqn_;
    const long double scaledEnd =
        static_cast<long double>(phaseAtEnd) * detectorPpqn_;
    if (!std::isfinite(phaseAtEnd) || !(scaledEnd > scaledStart)) {
        return;
    }

    const long double firstValue = std::ceil(scaledStart);
    const long double endExclusiveValue = std::ceil(scaledEnd);
    constexpr long double minimumCell =
        static_cast<long double>(std::numeric_limits<std::int64_t>::min());
    constexpr long double maximumCell =
        static_cast<long double>(std::numeric_limits<std::int64_t>::max());
    if (firstValue < minimumCell || firstValue > maximumCell ||
        endExclusiveValue < minimumCell || endExclusiveValue > maximumCell) {
        ++droppedOutputCount_;
        return;
    }

    const auto firstCell = static_cast<std::int64_t>(firstValue);
    const auto endExclusive = static_cast<std::int64_t>(endExclusiveValue);
    if (endExclusive <= firstCell) {
        return;
    }
    const std::uint64_t candidateCount =
        static_cast<std::uint64_t>(endExclusive - firstCell);
    const std::uint64_t processCount = std::min<std::uint64_t>(
        candidateCount,
        remainingCandidateIterations);
    remainingCandidateIterations -= static_cast<std::size_t>(processCount);

    for (std::uint64_t offset = 0; offset < processCount; ++offset) {
        const std::int64_t cell = firstCell + static_cast<std::int64_t>(offset);
        const double boundaryQuarterNotes =
            static_cast<double>(cell) / static_cast<double>(detectorPpqn_);
        const double sample = startSample +
            (boundaryQuarterNotes - phaseAtStart) / quarterNotesPerSample;
        if (!(sample >= startSample && sample < endSample)) {
            continue;
        }
        const double detectorTime = std::nextafter(
            boundaryQuarterNotes,
            std::numeric_limits<double>::infinity());
        if (!outputTickDetector_.Process(Phasor2Tick::Input{
                .time = detectorTime,
                .multiplier = detectorPpqn_,
            })) {
            continue;
        }
        ++enumeratedCrossingCount_;
        const auto dueTime = DueTimeAtSample(sample);
        if (!dueTime.has_value()) {
            ++droppedOutputCount_;
            continue;
        }
        EnqueueScheduledEvent(
            ScheduledMidiEventKind::TimingClock,
            *dueTime,
            phaseGeneration_);
    }

    if (candidateCount > processCount) {
        const std::uint64_t skipped = candidateCount - processCount;
        droppedOutputCount_ += skipped;
        const std::int64_t finalCell = endExclusive - 1;
        const double finalBoundary =
            static_cast<double>(finalCell) / static_cast<double>(detectorPpqn_);
        outputTickDetector_.Process(Phasor2Tick::Input{
            .time = std::nextafter(finalBoundary, std::numeric_limits<double>::infinity()),
            .multiplier = detectorPpqn_,
        });
    }
}

void MasterClock::EnumerateCurrentPlanCrossings() noexcept {
    if (!hasCurrentPlan_) {
        return;
    }
    const ClockPlanDescriptor& descriptor = currentPlan_.Descriptor();
    const double phaseStart = descriptor.transportState == ClockTransportState::Running
        ? descriptor.transportStartQuarterNotes
        : descriptor.lifetimeStartQuarterNotes;
    std::size_t remainingCandidateIterations = kMaximumCrossingsPerSegment;
    EnumerateAffineCrossings(
        static_cast<double>(descriptor.startSample),
        static_cast<double>(descriptor.endSample),
        phaseStart,
        descriptor.quarterNotesPerSample,
        remainingCandidateIterations);
}

std::optional<double> MasterClock::LifetimeQuarterNotesAtKnownSample(
    double sample) const noexcept {
    const auto point = TimeAtSample(sample);
    if (point.has_value()) {
        return point->lifetimeQuarterNotes;
    }
    if (hasCurrentPlan_ && std::isfinite(sample) &&
        sample == static_cast<double>(expectedNextSample_)) {
        return nextLifetimeQuarterNotes_;
    }
    return std::nullopt;
}

void MasterClock::EnumerateSpliceCrossings(
    double spliceSample,
    double phaseAtSplice,
    bool useLifetimePhase) noexcept {
    if (!hasCurrentPlan_ || !std::isfinite(spliceSample)) {
        return;
    }
    const auto lifetimeAtSplice = LifetimeQuarterNotesAtKnownSample(spliceSample);
    if (!lifetimeAtSplice.has_value()) {
        return;
    }
    // One budget covers the entire retained-history splice. Descriptors are
    // oldest-first, so excess newest candidates are observable drops; each
    // affine segment still advances detector authority to its final cell.
    std::size_t remainingCandidateIterations = kMaximumSpliceCrossingIterations;
    for (std::size_t logical = 0; logical < planHistoryCount_; ++logical) {
        const ClockPlanDescriptor& descriptor = PlanAtLogicalIndex(logical);
        const double segmentStart = std::max(
            spliceSample,
            static_cast<double>(descriptor.startSample));
        const double segmentEnd = static_cast<double>(descriptor.endSample);
        if (!(segmentStart < segmentEnd)) {
            continue;
        }
        const double lifetimeAtSegmentStart =
            descriptor.LifetimeQuarterNotesAt(segmentStart);
        const double phaseStart = useLifetimePhase
            ? lifetimeAtSegmentStart
            : phaseAtSplice + lifetimeAtSegmentStart - *lifetimeAtSplice;
        EnumerateAffineCrossings(
            segmentStart,
            segmentEnd,
            phaseStart,
            descriptor.quarterNotesPerSample,
            remainingCandidateIterations);
    }
}

void MasterClock::ActivateTransportAtExternalTimestamp(
    std::uint64_t timestampMicros,
    std::optional<ClockTransportCommand> transportAtSameTimestamp) noexcept {
    const auto dueTime = DueTimeFromTimestamp(timestampMicros);
    BeginPhaseGeneration(timestampMicros);
    if (transportAtSameTimestamp.has_value() && dueTime.has_value()) {
        EmitTransport(*transportAtSameTimestamp, *dueTime);
    }

    const auto spliceSample = SampleAtTimestamp(timestampMicros);
    const auto lifetimeAtActivation = spliceSample.has_value()
        ? LifetimeQuarterNotesAtKnownSample(*spliceSample)
        : std::nullopt;
    transportState_ = ClockTransportState::Running;
    // Safe fallback for a violated latency contract: if the activation lies
    // outside retained committed history, start the next committed run at zero
    // and skip retroactive reconstruction instead of extrapolating unknown phase.
    nextTransportQuarterNotes_ = lifetimeAtActivation.has_value()
        ? std::max(0.0, nextLifetimeQuarterNotes_ - *lifetimeAtActivation)
        : 0.0;
    ++transportEpoch_;
    PrimeOutputDetector(0.0);
    if (dueTime.has_value()) {
        EmitExplicitZeroClock(*dueTime);
    } else {
        ++droppedOutputCount_;
    }
    if (spliceSample.has_value()) {
        EnumerateSpliceCrossings(*spliceSample, 0.0, false);
    }
}

void MasterClock::StopTransportAtExternalTimestamp(
    std::uint64_t timestampMicros) noexcept {
    const bool gridWasRunning = transportState_ == ClockTransportState::Running;
    const auto dueTime = DueTimeFromTimestamp(timestampMicros);
    if (gridWasRunning) {
        BeginPhaseGeneration(timestampMicros);
    }
    if (dueTime.has_value()) {
        EmitTransport(ClockTransportCommand::Stop, *dueTime);
    } else {
        ++droppedOutputCount_;
    }

    const auto spliceSample = SampleAtTimestamp(timestampMicros);
    const auto lifetimeAtStop = spliceSample.has_value()
        ? LifetimeQuarterNotesAtKnownSample(*spliceSample)
        : std::nullopt;
    transportState_ = ClockTransportState::Stopped;
    nextTransportQuarterNotes_ = 0.0;
    if (gridWasRunning && lifetimeAtStop.has_value()) {
        PrimeOutputDetector(*lifetimeAtStop);
        EnumerateSpliceCrossings(*spliceSample, *lifetimeAtStop, true);
    }
}

void MasterClock::ApplyOutputConfigurationAtBoundary(
    std::uint64_t boundaryTimestampMicros) noexcept {
    if (!outputConfigurationPending_) {
        return;
    }
    if (hasCurrentPlan_) {
        BeginPhaseGeneration(boundaryTimestampMicros);
    }
    detectorPpqn_ = syncConfig_.ppqn;
    PrimeOutputDetector(SelectedPhaseAtNextBoundary());
    outputConfigurationPending_ = false;
}

void MasterClock::ApplyPendingInternalTransport(
    std::uint64_t boundaryTimestampMicros) noexcept {
    if (!hasPendingInternalTransport_) {
        return;
    }
    const ClockTransportCommand finalCommand = pendingInternalTransport_;
    const bool startCommand = IsStartCommand(finalCommand);
    const bool activatesNow = startCommand && !syncConfig_.receiveClock;
    const bool entersArmed = startCommand && syncConfig_.receiveClock;
    const bool gridWasRunning = transportState_ == ClockTransportState::Running;
    const bool switchesGrid = activatesNow ||
        (entersArmed && gridWasRunning) ||
        (finalCommand == ClockTransportCommand::Stop && gridWasRunning);
    if (switchesGrid) {
        BeginPhaseGeneration(boundaryTimestampMicros);
    }

    const auto dueTime = DueTimeFromTimestamp(boundaryTimestampMicros);
    if (dueTime.has_value()) {
        for (std::size_t index = 0;
             index < pendingInternalTransportOutputCount_;
             ++index) {
            EmitTransport(pendingInternalTransportOutputs_[index], *dueTime);
        }
    } else {
        droppedOutputCount_ += pendingInternalTransportOutputCount_;
    }

    if (activatesNow) {
        transportState_ = ClockTransportState::Running;
        nextTransportQuarterNotes_ = 0.0;
        ++transportEpoch_;
        PrimeOutputDetector(0.0);
        if (dueTime.has_value()) {
            EmitExplicitZeroClock(*dueTime);
        }
    } else if (entersArmed) {
        transportState_ = finalCommand == ClockTransportCommand::Start
            ? ClockTransportState::ArmedStart
            : ClockTransportState::ArmedContinue;
        nextTransportQuarterNotes_ = 0.0;
        if (gridWasRunning) {
            PrimeOutputDetector(nextLifetimeQuarterNotes_);
        }
    } else {
        transportState_ = ClockTransportState::Stopped;
        nextTransportQuarterNotes_ = 0.0;
        if (gridWasRunning) {
            PrimeOutputDetector(nextLifetimeQuarterNotes_);
        }
    }

    hasPendingInternalTransport_ = false;
    pendingInternalTransportOutputCount_ = 0;
}

bool MasterClock::Prepare(double sampleRate, std::size_t blockSize) noexcept {
    if (!IsFinitePositive(sampleRate) || blockSize == 0) {
        return false;
    }

    const double blockDurationMicros =
        static_cast<double>(blockSize) * 1'000'000.0 / sampleRate;
    const double baseLatency = std::max(2.0 * blockDurationMicros, 5000.0);
    if (!std::isfinite(baseLatency) ||
        baseLatency > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return false;
    }
    const auto baseLatencyMicros = static_cast<std::uint64_t>(std::ceil(baseLatency));
    if (baseLatencyMicros > std::numeric_limits<std::uint64_t>::max() -
                                outputSchedulingHorizonMicros_) {
        return false;
    }
    const auto outputLatencyMicros = baseLatencyMicros + outputSchedulingHorizonMicros_;
    const double quarterNotesPerSample = manualBpm_ / (60.0 * sampleRate);
    if (!IsFinitePositive(quarterNotesPerSample)) {
        return false;
    }

    AudioSampleTimeMapper mapper;
    if (!mapper.Prepare(sampleRate, outputLatencyMicros)) {
        return false;
    }

    sampleRate_ = sampleRate;
    blockSize_ = blockSize;
    outputLatencyMicros_ = outputLatencyMicros;
    activeBpm_ = manualBpm_;
    pendingQuarterNotesPerSample_ = quarterNotesPerSample;
    nextLifetimeQuarterNotes_ = 0.0;
    nextTransportQuarterNotes_ = 0.0;
    transportState_ = ClockTransportState::Stopped;
    transportEpoch_ = 0;
    planGeneration_ = 1;
    expectedNextSample_ = 0;
    planHistoryHead_ = 0;
    planHistoryCount_ = 0;
    currentPlan_ = ClockBlockPlan{};
    hasCurrentPlan_ = false;
    timeMapper_ = mapper;
    ignoredInputCount_ = 0;
    droppedOutputCount_ = 0;
    scheduledSequence_ = 0;
    phaseGeneration_ = 1;
    enumeratedCrossingCount_ = 0;
    sourceTimeoutCount_ = 0;
    pendingInternalTransportOutputCount_ = 0;
    hasPendingInternalTransport_ = false;
    hasActiveExternalSource_ = false;
    sourceIsProvisional_ = false;
    activeExternalSourceSlot_ = 0;
    activeSourceLastActivityMicros_ = 0;
    externalEstimator_ = {};
    detectorPpqn_ = syncConfig_.ppqn;
    outputTickDetector_ = Phasor2Tick{};
    PrimeOutputDetector(0.0);
    outputConfigurationPending_ = false;
    prepared_ = true;
    acquisitionState_ = syncConfig_.receiveClock
        ? ClockAcquisitionState::Acquiring
        : ClockAcquisitionState::Internal;
    source_ = syncConfig_.receiveClock ? ClockSource::ExternalMidi : ClockSource::Internal;
    return true;
}

bool MasterClock::SetTempoBpm(double bpm) noexcept {
    if (!IsFinitePositive(bpm) || syncConfig_.receiveClock) {
        return false;
    }
    double nextQuarterNotesPerSample = pendingQuarterNotesPerSample_;
    if (prepared_) {
        nextQuarterNotesPerSample = bpm / (60.0 * sampleRate_);
        if (!IsFinitePositive(nextQuarterNotesPerSample)) {
            return false;
        }
    }
    manualBpm_ = bpm;
    activeBpm_ = bpm;
    if (prepared_) {
        pendingQuarterNotesPerSample_ = nextQuarterNotesPerSample;
    }
    return true;
}

double MasterClock::LifetimeQuarterNotes() const noexcept {
    return hasCurrentPlan_ ? currentPlan_.LifetimeStartQuarterNotes() : nextLifetimeQuarterNotes_;
}

double MasterClock::TransportQuarterNotes() const noexcept {
    return hasCurrentPlan_ ? currentPlan_.TransportStartQuarterNotes() : 0.0;
}

bool MasterClock::ApplySyncConfig(const SyncConfig& config) noexcept {
    if (!config.IsValid()) {
        return false;
    }
    const bool receiveChanged = config.receiveClock != syncConfig_.receiveClock;
    const bool ppqnChanged = config.ppqn != syncConfig_.ppqn;
    const bool outputGridChanged =
        ppqnChanged || config.sendClock != syncConfig_.sendClock;
    double restoredQuarterNotesPerSample = pendingQuarterNotesPerSample_;
    if (receiveChanged && !config.receiveClock && prepared_) {
        restoredQuarterNotesPerSample = manualBpm_ / (60.0 * sampleRate_);
        if (!IsFinitePositive(restoredQuarterNotesPerSample)) {
            return false;
        }
    }
    syncConfig_ = config;
    if (receiveChanged || ppqnChanged) {
        ClearExternalSource(false);
    }
    if (receiveChanged && syncConfig_.receiveClock) {
        acquisitionState_ = ClockAcquisitionState::Acquiring;
        source_ = ClockSource::ExternalMidi;
    } else if (receiveChanged) {
        acquisitionState_ = ClockAcquisitionState::Internal;
        source_ = ClockSource::Internal;
        activeBpm_ = manualBpm_;
        if (prepared_) {
            pendingQuarterNotesPerSample_ = restoredQuarterNotesPerSample;
        }
    } else if (ppqnChanged && syncConfig_.receiveClock) {
        acquisitionState_ = ClockAcquisitionState::Acquiring;
        source_ = ClockSource::ExternalMidi;
    }
    if (prepared_ && outputGridChanged) {
        outputConfigurationPending_ = true;
    }
    return true;
}

bool MasterClock::HandleInternalTransport(ClockTransportCommand command) noexcept {
    pendingInternalTransport_ = command;
    hasPendingInternalTransport_ = true;
    if (pendingInternalTransportOutputCount_ < kPendingInternalTransportCapacity) {
        pendingInternalTransportOutputs_[pendingInternalTransportOutputCount_++] = command;
    } else {
        ++droppedOutputCount_;
    }
    return true;
}

bool MasterClock::HandleExternalTransport(
    ClockTransportCommand command,
    std::uint64_t timestampMicros,
    std::size_t controllerSlot) noexcept {
    if (!syncConfig_.receiveTransport) {
        ++ignoredInputCount_;
        return false;
    }
    const bool startCommand = IsStartCommand(command);
    if (!AcceptExternalSource(
            controllerSlot,
            timestampMicros,
            startCommand,
            startCommand)) {
        return false;
    }
    if (startCommand && sourceIsProvisional_) {
        activeSourceLastActivityMicros_ = timestampMicros;
    }

    if (startCommand && syncConfig_.receiveClock) {
        const bool gridWasRunning = transportState_ == ClockTransportState::Running;
        const auto dueTime = DueTimeFromTimestamp(timestampMicros);
        if (gridWasRunning) {
            BeginPhaseGeneration(timestampMicros);
        }
        if (dueTime.has_value()) {
            EmitTransport(command, *dueTime);
        } else {
            ++droppedOutputCount_;
        }
        transportState_ = command == ClockTransportCommand::Start
            ? ClockTransportState::ArmedStart
            : ClockTransportState::ArmedContinue;
        nextTransportQuarterNotes_ = 0.0;
        if (gridWasRunning) {
            const auto spliceSample = SampleAtTimestamp(timestampMicros);
            const auto lifetime = spliceSample.has_value()
                ? LifetimeQuarterNotesAtKnownSample(*spliceSample)
                : std::nullopt;
            if (lifetime.has_value()) {
                PrimeOutputDetector(*lifetime);
                EnumerateSpliceCrossings(*spliceSample, *lifetime, true);
            }
        }
        return true;
    }

    if (startCommand) {
        ActivateTransportAtExternalTimestamp(timestampMicros, command);
        return true;
    }

    StopTransportAtExternalTimestamp(timestampMicros);
    return true;
}

bool MasterClock::HandleExternalClock(
    std::uint64_t timestampMicros,
    std::size_t controllerSlot) noexcept {
    if (!syncConfig_.receiveClock) {
        ++ignoredInputCount_;
        return false;
    }
    if (!AcceptExternalSource(
            controllerSlot,
            timestampMicros,
            true,
            false)) {
        return false;
    }
    if (sourceIsProvisional_) {
        sourceIsProvisional_ = false;
    }

    if (!externalEstimator_.hasLastClock) {
        externalEstimator_.hasLastClock = true;
        externalEstimator_.lastClockTimestampMicros = timestampMicros;
        externalEstimator_.inputTickIndex = 0;
        ++externalEstimator_.acceptedClockCount;
        activeSourceLastActivityMicros_ = timestampMicros;
        const auto point = TimeAtTimestamp(timestampMicros);
        if (point.has_value()) {
            externalEstimator_.phaseAnchorQuarterNotes = point->lifetimeQuarterNotes;
            externalEstimator_.hasPhaseAnchor = true;
        }
        if (transportState_ == ClockTransportState::ArmedStart ||
            transportState_ == ClockTransportState::ArmedContinue) {
            ActivateTransportAtExternalTimestamp(timestampMicros);
        }
        return true;
    }

    if (timestampMicros <= externalEstimator_.lastClockTimestampMicros) {
        ++ignoredInputCount_;
        return false;
    }
    const double rawInterval = static_cast<double>(
        timestampMicros - externalEstimator_.lastClockTimestampMicros);
    std::uint64_t elapsedPulses = 1;
    double normalizedInterval = rawInterval;
    if (externalEstimator_.hasPeriodEstimate) {
        const double ratio = rawInterval / externalEstimator_.filteredPeriodMicros;
        const auto rounded = static_cast<std::int64_t>(std::llround(ratio));
        elapsedPulses = static_cast<std::uint64_t>(std::clamp<std::int64_t>(rounded, 1, 8));
        normalizedInterval = rawInterval / static_cast<double>(elapsedPulses);
        const double relativeError = std::fabs(
            normalizedInterval - externalEstimator_.filteredPeriodMicros) /
            externalEstimator_.filteredPeriodMicros;
        if (!std::isfinite(relativeError) || relativeError > kMissedPulseTolerance) {
            ++ignoredInputCount_;
            return false;
        }
    }

    externalEstimator_.rawIntervalMicros = rawInterval;
    PushExternalInterval(normalizedInterval);
    externalEstimator_.medianIntervalMicros = MedianExternalInterval();
    if (!externalEstimator_.hasPeriodEstimate) {
        externalEstimator_.filteredPeriodMicros = normalizedInterval;
        externalEstimator_.hasPeriodEstimate = true;
    } else {
        externalEstimator_.filteredPeriodMicros +=
            (externalEstimator_.medianIntervalMicros -
             externalEstimator_.filteredPeriodMicros) *
            kExternalPeriodEwmaGain;
    }
    externalEstimator_.lastClockTimestampMicros = timestampMicros;
    externalEstimator_.inputTickIndex += elapsedPulses;
    ++externalEstimator_.validIntervalCount;
    ++externalEstimator_.acceptedClockCount;
    externalEstimator_.inferredMissedPulseCount += elapsedPulses - 1;
    activeSourceLastActivityMicros_ = timestampMicros;

    const double recoveredBpm =
        60'000'000.0 /
        (static_cast<double>(syncConfig_.ppqn) *
         externalEstimator_.filteredPeriodMicros);
    if (!IsFinitePositive(recoveredBpm)) {
        ++ignoredInputCount_;
        return false;
    }
    activeBpm_ = recoveredBpm;
    acquisitionState_ = externalEstimator_.validIntervalCount >= 2
        ? ClockAcquisitionState::Locked
        : ClockAcquisitionState::Acquiring;
    ApplyExternalPhaseObservation(timestampMicros);

    if (transportState_ == ClockTransportState::ArmedStart ||
        transportState_ == ClockTransportState::ArmedContinue) {
        ActivateTransportAtExternalTimestamp(timestampMicros);
    }
    return true;
}

ClockDiagnostics MasterClock::DiagnosticsSnapshot() const noexcept {
    const auto mapperDiagnostics = timeMapper_.DiagnosticsSnapshot();
    return ClockDiagnostics{
        .acquisition = acquisitionState_,
        .source = source_,
        .hasActiveExternalSource = hasActiveExternalSource_,
        .activeExternalSourceSlot = activeExternalSourceSlot_,
        .transportState = TransportState(),
        .currentBpm = activeBpm_,
        .outputLatencyMicros = outputLatencyMicros_,
        .ignoredInputCount = ignoredInputCount_,
        .mapperIgnoredObservationCount = mapperDiagnostics.ignoredObservationCount,
        .lateEventCount = mapperDiagnostics.lateEventCount,
        .droppedOutputCount = droppedOutputCount_,
        .mapperDiscontinuityCount = mapperDiagnostics.discontinuityCount,
        .mapperGeneration = mapperDiagnostics.generation,
        .sourceIsProvisional = sourceIsProvisional_,
        .acceptedExternalClockCount = externalEstimator_.acceptedClockCount,
        .externalInputTickIndex = externalEstimator_.inputTickIndex,
        .inferredMissedPulseCount = externalEstimator_.inferredMissedPulseCount,
        .hardReacquireCount = externalEstimator_.hardReacquireCount,
        .sourceTimeoutCount = sourceTimeoutCount_,
        .enumeratedCrossingCount = enumeratedCrossingCount_,
        .phaseGeneration = phaseGeneration_,
        .externalRawIntervalMicros = externalEstimator_.rawIntervalMicros,
        .externalMedianIntervalMicros = externalEstimator_.medianIntervalMicros,
        .externalFilteredPeriodMicros = externalEstimator_.filteredPeriodMicros,
        .externalPhaseErrorPulsePeriods = externalEstimator_.phaseErrorPulsePeriods,
        .appliedPhaseCorrectionPulsePeriods =
            externalEstimator_.appliedPhaseCorrectionPulsePeriods,
    };
}

const ClockBlockPlan* MasterClock::CommitBlock(
    std::uint64_t blockStartSample,
    std::size_t frameCount,
    std::uint64_t callbackTimestampMicros) noexcept {
    if (!prepared_ || frameCount == 0 ||
        frameCount > std::numeric_limits<std::uint64_t>::max() - blockStartSample ||
        (hasCurrentPlan_ && blockStartSample != expectedNextSample_) ||
        !IsFinitePositive(pendingQuarterNotesPerSample_)) {
        return nullptr;
    }
    const std::uint64_t endSample = blockStartSample + static_cast<std::uint64_t>(frameCount);
    ExpireExternalSource(callbackTimestampMicros);
    const MapperObservationResult observation =
        timeMapper_.ObserveBlock(blockStartSample, callbackTimestampMicros);
    if (observation == MapperObservationResult::Rejected) {
        return nullptr;
    }
    const auto boundaryTimestamp =
        timeMapper_.TimestampMicrosAt(static_cast<double>(blockStartSample));
    if (!boundaryTimestamp.has_value()) {
        return nullptr;
    }
    if (observation == MapperObservationResult::Discontinuity && hasCurrentPlan_) {
        BeginPhaseGeneration(*boundaryTimestamp);
        PrimeOutputDetector(SelectedPhaseAtNextBoundary());
    }
    ApplyOutputConfigurationAtBoundary(*boundaryTimestamp);
    ApplyPendingInternalTransport(*boundaryTimestamp);

    const ClockPlanDescriptor descriptor{
        .startSample = blockStartSample,
        .endSample = endSample,
        .lifetimeStartQuarterNotes = nextLifetimeQuarterNotes_,
        .transportStartQuarterNotes = nextTransportQuarterNotes_,
        .quarterNotesPerSample = pendingQuarterNotesPerSample_,
        .transportState = transportState_,
        .transportEpoch = transportEpoch_,
        .generation = planGeneration_,
    };
    currentPlan_ = ClockBlockPlan{descriptor};
    hasCurrentPlan_ = true;
    PushPlan(descriptor);
    EnumerateCurrentPlanCrossings();
    nextLifetimeQuarterNotes_ = descriptor.LifetimeEndQuarterNotes();
    nextTransportQuarterNotes_ = descriptor.TransportEndQuarterNotes();
    expectedNextSample_ = endSample;
    return &currentPlan_;
}

std::optional<ClockBlockPlan> MasterClock::PlanAtSample(
    double absoluteOutputSample) const noexcept {
    if (!std::isfinite(absoluteOutputSample)) {
        return std::nullopt;
    }
    for (std::size_t logical = planHistoryCount_; logical > 0; --logical) {
        const ClockPlanDescriptor& descriptor = PlanAtLogicalIndex(logical - 1);
        if (descriptor.Contains(absoluteOutputSample)) {
            return ClockBlockPlan{descriptor};
        }
    }
    return std::nullopt;
}

std::optional<ClockTimePoint> MasterClock::TimeAtSample(
    double absoluteOutputSample) const noexcept {
    const auto plan = PlanAtSample(absoluteOutputSample);
    if (!plan.has_value()) {
        return std::nullopt;
    }
    return ClockTimePoint{
        .samplePosition = absoluteOutputSample,
        .lifetimeQuarterNotes = plan->LifetimeQuarterNotesAt(absoluteOutputSample),
        .transportQuarterNotes = plan->TransportQuarterNotesAt(absoluteOutputSample),
        .transportState = plan->TransportState(),
        .transportEpoch = plan->TransportEpoch(),
    };
}

std::optional<ClockTimePoint> MasterClock::TimeAtTimestamp(
    std::uint64_t timestampMicros) const noexcept {
    const auto sample = timeMapper_.SampleAtTimestamp(timestampMicros);
    if (!sample.has_value()) {
        return std::nullopt;
    }
    return TimeAtSample(*sample);
}

const ClockPlanDescriptor& MasterClock::PlanAtLogicalIndex(
    std::size_t logicalIndex) const noexcept {
    return planHistory_[(planHistoryHead_ + logicalIndex) % kPlanHistoryCapacity];
}

void MasterClock::PushPlan(const ClockPlanDescriptor& descriptor) noexcept {
    if (planHistoryCount_ < kPlanHistoryCapacity) {
        planHistory_[(planHistoryHead_ + planHistoryCount_) % kPlanHistoryCapacity] = descriptor;
        ++planHistoryCount_;
        return;
    }
    planHistory_[planHistoryHead_] = descriptor;
    planHistoryHead_ = (planHistoryHead_ + 1) % kPlanHistoryCapacity;
}

} // namespace synth
