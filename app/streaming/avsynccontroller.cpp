#include "avsynccontroller.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <limits>

namespace PlankAvSync {
namespace {
constexpr std::int32_t MinimumUpdateIntervalMs = 1000;
constexpr double MinimumFitDurationMs = 120000.0;
constexpr double FitWindowDurationMs = 300000.0;
constexpr std::int32_t MaximumVideoClockAgeMs = 2000;
constexpr int MaximumCorrectionStepPpm = 2;
constexpr double PhaseCorrectionHorizonMs = 1800000.0;
constexpr std::uint32_t PhaseUpdateIntervalMs = 100;
constexpr double PhaseFilterTimeMs = 1000.0;
constexpr double PhaseDeadbandUs = 5000.0;
constexpr double PhaseGainPpmPerMs = 100.0;
constexpr int MaximumPhaseStepPpmPerSecond = 10000;

std::mutex videoClockLock;
VideoClockSample latestVideoClock;
}

void resetVideoClock()
{
    std::lock_guard<std::mutex> lock(videoClockLock);
    latestVideoClock = {};
}

void publishVideoClock(std::int64_t mediaTimeMs,
                       std::uint32_t presentationTicks)
{
    std::lock_guard<std::mutex> lock(videoClockLock);
    latestVideoClock.mediaTimeMs = mediaTimeMs;
    latestVideoClock.presentationTicks = presentationTicks;
    latestVideoClock.valid = true;
}

VideoClockSample readVideoClock()
{
    std::lock_guard<std::mutex> lock(videoClockLock);
    return latestVideoClock;
}

void AudioTimestampObserver::reset()
{
    m_ExpectedSourceUs = -1;
    m_Discontinuities = 0;
}

AudioTimestampObserver::Observation AudioTimestampObserver::observe(
        std::int64_t sourceUs, int frames, int sampleRate,
        std::uint32_t nowMs, int queuedMs, int deviceMs,
        std::int64_t resamplerDelayUs, const VideoClockSample& video,
        bool commonEpoch)
{
    Observation result;
    result.discontinuities = m_Discontinuities;
    if (sourceUs < 0 || frames <= 0 || sampleRate <= 0) {
        m_ExpectedSourceUs = -1;
        return result;
    }
    const auto durationUs = std::int64_t(frames) * 1000000 / sampleRate;
    if (sourceUs > std::numeric_limits<std::int64_t>::max() - durationUs) {
        m_ExpectedSourceUs = -1;
        return result;
    }
    result.sourceValid = true;
    result.sourceUs = sourceUs;
    if (m_ExpectedSourceUs >= 0) {
        result.sourceGapUs = sourceUs - m_ExpectedSourceUs;
        // Audio wire PTS is rounded down to milliseconds. Do not call its
        // sub-millisecond quantization a capture discontinuity.
        if (result.sourceGapUs > 1000 || result.sourceGapUs < -1000)
            result.discontinuities = ++m_Discontinuities;
    }
    m_ExpectedSourceUs = sourceUs + durationUs;
    const auto ageMs = static_cast<std::int32_t>(nowMs - video.presentationTicks);
    if (!commonEpoch || !video.valid || video.mediaTimeMs < 0 ||
            video.mediaTimeMs > std::numeric_limits<std::int64_t>::max() / 1000 - 10000 ||
            ageMs < 0 || ageMs > MaximumVideoClockAgeMs ||
            queuedMs < 0 || queuedMs > 2000 || deviceMs < 0 || deviceMs > 2000 ||
            resamplerDelayUs < 0 || resamplerDelayUs > 2000000) return result;
    result.videoUs = (video.mediaTimeMs + ageMs) * 1000;
    // At this enqueue boundary the new output starts after the queued audio
    // and device buffer. swresample may still hold earlier source samples.
    result.estimatedLeadUs = sourceUs - result.videoUs - resamplerDelayUs -
        std::int64_t(queuedMs + deviceMs) * 1000;
    result.phaseValid = true;
    return result;
}

void AudioRateController::reset()
{
    m_AudioPoints.clear();
    m_VideoPoints.clear();
    m_FirstAudioFrames = 0;
    m_FirstSubmittedAudioFrames = 0;
    m_FirstVideoMediaMs = 0;
    m_FirstAudioObservation = 0;
    m_FirstVideoPresentation = 0;
    m_LastAudioObservation = 0;
    m_LastVideoPresentation = 0;
    m_CorrectionPpm = 0;
    m_Anchored = false;
}

AudioRateController::Result AudioRateController::update(
        std::uint64_t rawAudioFrames,
        std::uint64_t submittedAudioFrames,
        int sampleRate,
        std::uint32_t audioObservationTicks,
        const VideoClockSample& videoClock)
{
    Result result {m_CorrectionPpm, false};
    if (sampleRate <= 0 || !videoClock.valid) {
        return result;
    }

    const std::int32_t videoClockAge = static_cast<std::int32_t>(
        audioObservationTicks - videoClock.presentationTicks);
    if (std::abs(videoClockAge) > MaximumVideoClockAgeMs) {
        return result;
    }

    if (!m_Anchored) {
        m_FirstAudioFrames = rawAudioFrames;
        m_FirstSubmittedAudioFrames = submittedAudioFrames;
        m_FirstVideoMediaMs = videoClock.mediaTimeMs;
        m_FirstAudioObservation = audioObservationTicks;
        m_FirstVideoPresentation = videoClock.presentationTicks;
        m_LastAudioObservation = audioObservationTicks;
        m_LastVideoPresentation = videoClock.presentationTicks;
        m_AudioPoints.push_back({0.0, 0.0});
        m_VideoPoints.push_back({0.0, 0.0});
        m_Anchored = true;
        return result;
    }

    if (static_cast<std::int32_t>(audioObservationTicks -
                                  m_LastAudioObservation) <
            MinimumUpdateIntervalMs) {
        return result;
    }
    m_LastAudioObservation = audioObservationTicks;

    const double audioObservationElapsed = static_cast<std::uint32_t>(
        audioObservationTicks - m_FirstAudioObservation);
    const double audioMediaElapsed =
        static_cast<double>(rawAudioFrames - m_FirstAudioFrames) * 1000.0 /
        sampleRate;
    m_AudioPoints.push_back({audioObservationElapsed, audioMediaElapsed});

    if (videoClock.presentationTicks != m_LastVideoPresentation) {
        const double videoPresentationElapsed = static_cast<std::uint32_t>(
            videoClock.presentationTicks - m_FirstVideoPresentation);
        const double videoMediaElapsed = static_cast<double>(
            videoClock.mediaTimeMs - m_FirstVideoMediaMs);
        if (videoMediaElapsed < 0.0) {
            reset();
            return result;
        }
        m_VideoPoints.push_back({videoPresentationElapsed, videoMediaElapsed});
        m_LastVideoPresentation = videoClock.presentationTicks;
    }

    trimWindow(m_AudioPoints);
    trimWindow(m_VideoPoints);
    if (m_AudioPoints.size() < 20 || m_VideoPoints.size() < 20 ||
            m_AudioPoints.back().clockMs -
                m_AudioPoints.front().clockMs < MinimumFitDurationMs ||
            m_VideoPoints.back().clockMs -
                m_VideoPoints.front().clockMs < MinimumFitDurationMs) {
        return result;
    }

    const double audioRate = fitRate(m_AudioPoints);
    const double videoRate = fitRate(m_VideoPoints);
    if (audioRate <= 0.0 || videoRate <= 0.0) {
        return result;
    }

    const double submittedAudioMediaElapsed =
        static_cast<double>(submittedAudioFrames -
                            m_FirstSubmittedAudioFrames) * 1000.0 /
        sampleRate;
    const double videoPresentationElapsed =
        m_VideoPoints.back().clockMs;
    const double videoMediaElapsed = m_VideoPoints.back().mediaMs;
    const double relativePhaseErrorMs =
        (audioObservationElapsed - submittedAudioMediaElapsed) -
        (videoPresentationElapsed - videoMediaElapsed);
    const int phaseCorrectionPpm = static_cast<int>(std::llround(
        relativePhaseErrorMs * 1000000.0 / PhaseCorrectionHorizonMs));
    const int measuredPpm = std::clamp(
        static_cast<int>(std::llround((audioRate / videoRate - 1.0) * 1000000.0)),
        -MaximumCorrectionPpm,
        MaximumCorrectionPpm);
    const int targetPpm = std::clamp(measuredPpm - phaseCorrectionPpm,
                                     -MaximumCorrectionPpm,
                                     MaximumCorrectionPpm);
    const int delta = std::clamp(targetPpm - m_CorrectionPpm,
                                 -MaximumCorrectionStepPpm,
                                 MaximumCorrectionStepPpm);
    m_CorrectionPpm += delta;
    if (std::abs(m_CorrectionPpm) < 2) {
        m_CorrectionPpm = 0;
    }

    result.correctionPpm = m_CorrectionPpm;
    result.updated = true;
    return result;
}

int AudioRateController::correctionPpm() const
{
    return m_CorrectionPpm;
}

double AudioRateController::fitRate(const std::deque<ClockPoint>& points)
{
    double meanClock = 0.0;
    double meanMedia = 0.0;
    for (const ClockPoint& point : points) {
        meanClock += point.clockMs;
        meanMedia += point.mediaMs;
    }
    meanClock /= points.size();
    meanMedia /= points.size();

    double covariance = 0.0;
    double variance = 0.0;
    for (const ClockPoint& point : points) {
        const double clockDelta = point.clockMs - meanClock;
        covariance += clockDelta * (point.mediaMs - meanMedia);
        variance += clockDelta * clockDelta;
    }
    return variance == 0.0 ? 0.0 : covariance / variance;
}

void AudioRateController::trimWindow(std::deque<ClockPoint>& points)
{
    while (points.size() > 2 &&
           points.back().clockMs - points.front().clockMs >
               FitWindowDurationMs) {
        points.pop_front();
    }
}

void AudioPlaybackObserver::observePull(int queuedBytes, int additionalBytes,
                                       int requestedBytes, int bytesPerSecond,
                                       std::uint32_t ticks)
{
    if (queuedBytes < 0 || additionalBytes < 0 || requestedBytes <= 0 ||
            bytesPerSecond <= 0) {
        m_Observation.headroomUs = -1;
        return;
    }
    ++m_Observation.pulls;
    m_Observation.ticks = ticks;
    const auto requestUs = std::int64_t(requestedBytes) * 1000000 / bytesPerSecond;
    m_Observation.requestUs = static_cast<int>(std::min<std::int64_t>(requestUs, INT32_MAX));
    // SDL's request is in the stream's input format, including conversion
    // requirements. additionalBytes may slightly overestimate a shortfall;
    // call this a shortage request, not a measured audible/device underrun.
    if (additionalBytes != 0) {
        ++m_Observation.shortageRequests;
        m_Observation.missingInputBytes += additionalBytes;
    }
    const auto headroomBytes = additionalBytes != 0 ? 0 :
        std::max(0, queuedBytes - requestedBytes);
    m_Observation.headroomUs = static_cast<int>(std::min<std::int64_t>(
        std::int64_t(headroomBytes) * 1000000 / bytesPerSecond, INT32_MAX));
}

void AudioPhaseController::reset()
{
    m_LastUpdateTicks = 0;
    m_LastObservationTicks = 0;
    m_FilteredLeadUs = 0.0;
    m_CorrectionPpm = 0;
    m_Anchored = false;
    m_LastOutputPull = 0;
    m_LastShortageRequests = 0;
    m_HealthyOutputPulls = 0;
    m_AccelerationBlocked = true;
}

AudioPhaseController::Result AudioPhaseController::update(
        const AudioTimestampObserver::Observation& timing,
        int queuedAudioMs,
        std::uint32_t observationTicks,
        const AudioPlaybackObserver::Observation& playback,
        int sourceBlockUs)
{
    // Unknown/stale video, capture discontinuities and invalid latency must
    // release compensation immediately, not leave an old resampling rate active.
    // Very large offsets require stream recovery, not minutes of rate chasing.
    if (!timing.sourceValid || !timing.phaseValid ||
            timing.estimatedLeadUs < -2000000 || timing.estimatedLeadUs > 2000000 ||
            timing.sourceGapUs < -100000 || timing.sourceGapUs > 100000 ||
            queuedAudioMs < 0 || queuedAudioMs > 2000 ||
            sourceBlockUs <= 0 || sourceBlockUs > 120000) {
        const bool changed = m_CorrectionPpm != 0;
        reset();
        return {0, changed};
    }
    // Protect the reserve at the actual output-pull boundary. An input queue
    // observed just before a large device pull can look healthy and still be
    // exhausted by that pull. Do not add a buffer or change the phase target.
    const auto outputAge = static_cast<std::uint32_t>(observationTicks - playback.ticks);
    const bool outputValid = playback.pulls != 0 && playback.headroomUs >= 0 &&
        playback.requestUs > 0 && playback.requestUs <= 2000000 &&
        outputAge <= static_cast<std::uint32_t>(std::max(100, playback.requestUs / 250));
    if (!outputValid) {
        m_AccelerationBlocked = true;
        m_HealthyOutputPulls = 0;
    }
    else if (playback.pulls != m_LastOutputPull) {
        const bool shortage = playback.shortageRequests != m_LastShortageRequests;
        if (shortage || playback.headroomUs < sourceBlockUs) {
            m_AccelerationBlocked = true;
            m_HealthyOutputPulls = 0;
        }
        else if (m_AccelerationBlocked) {
            // Resume only after three consecutive pulls retain two source
            // blocks. Avoid on/off correction at every producer callback.
            if (playback.pulls != m_LastOutputPull + 1)
                m_HealthyOutputPulls = 0;
            m_HealthyOutputPulls = playback.headroomUs >= 2 * sourceBlockUs ?
                m_HealthyOutputPulls + 1 : 0;
            if (m_HealthyOutputPulls >= 3)
                m_AccelerationBlocked = false;
        }
        m_LastOutputPull = playback.pulls;
        m_LastShortageRequests = playback.shortageRequests;
    }
    if (!m_Anchored) {
        m_LastUpdateTicks = observationTicks;
        m_LastObservationTicks = observationTicks;
        m_FilteredLeadUs = static_cast<double>(timing.estimatedLeadUs);
        m_Anchored = true;
        return {0, false};
    }

    const auto elapsedMs = static_cast<std::uint32_t>(observationTicks - m_LastUpdateTicks);
    const auto observationElapsedMs = static_cast<std::uint32_t>(
        observationTicks - m_LastObservationTicks);
    if (observationElapsedMs > 2000) {
        const bool changed = m_CorrectionPpm != 0;
        reset();
        return {0, changed};
    }
    // Filter every block, not just the 100ms actuator updates; subsampling here
    // can alias periodic video/USB callback jitter into a persistent phase bias.
    m_LastObservationTicks = observationTicks;
    const double alpha = 1.0 - std::exp(-static_cast<double>(observationElapsedMs) / PhaseFilterTimeMs);
    m_FilteredLeadUs += alpha * (timing.estimatedLeadUs - m_FilteredLeadUs);
    // Do not starve the output to chase unavoidable capture/device latency, or
    // add more buffering to an already full output queue. These guards also
    // apply between controller updates and bypass the ordinary slew limit.
    if ((m_CorrectionPpm > 0 && m_AccelerationBlocked) ||
            (m_CorrectionPpm < 0 && queuedAudioMs > 50)) {
        m_CorrectionPpm = 0;
        return {0, true};
    }
    if (elapsedMs < PhaseUpdateIntervalMs)
        return {m_CorrectionPpm, false};
    m_LastUpdateTicks = observationTicks;

    const double phaseErrorUs = std::copysign(
        std::max(0.0, std::abs(m_FilteredLeadUs) - PhaseDeadbandUs), m_FilteredLeadUs);
    // Audio behind (negative lead) needs positive/faster correction. Use the
    // actual source phase, not callback/sample-count throughput (which itself
    // changes as a consequence of output backpressure).
    int targetCorrection = std::clamp(
        static_cast<int>(std::llround(-phaseErrorUs / 1000.0 * PhaseGainPpmPerMs)),
        -MaximumCorrectionPpm, MaximumCorrectionPpm);
    if (m_AccelerationBlocked) {
        // Zero correction alone still drains the reserve when the device clock
        // is faster than the source. Recover headroom with the SAME bounded,
        // slew-limited resampler, not inserted silence or a second clock loop.
        // This is a safety ceiling on phase catch-up, not an A/V target offset.
        const int reserveErrorUs = outputValid ?
            std::min(0, playback.headroomUs - 2 * sourceBlockUs) : 0;
        const int safeCorrection = std::clamp(static_cast<int>(std::llround(
            reserveErrorUs / 1000.0 * PhaseGainPpmPerMs)), -MaximumCorrectionPpm, 0);
        targetCorrection = std::min(targetCorrection, safeCorrection);
    }
    if (targetCorrection < 0 && queuedAudioMs > 50)
        targetCorrection = 0;
    const int maximumStep = MaximumPhaseStepPpmPerSecond * std::min(elapsedMs, 1000U) / 1000;
    const int delta = std::clamp(targetCorrection - m_CorrectionPpm,
                                 -maximumStep, maximumStep);
    m_CorrectionPpm += delta;
    return {m_CorrectionPpm, true};
}

int AudioPhaseController::correctionPpm() const
{
    return m_CorrectionPpm;
}

} // namespace PlankAvSync
