#pragma once

#include <cstdint>
#include <deque>

namespace PlankAvSync {

struct VideoClockSample
{
    std::int64_t mediaTimeMs = 0;
    std::uint32_t presentationTicks = 0;
    bool valid = false;
};

void resetVideoClock();

void publishVideoClock(std::int64_t mediaTimeMs,
                       std::uint32_t presentationTicks);

VideoClockSample readVideoClock();

// Positive estimatedLeadUs means audio is ahead of
// video. Device/renderer timing is estimated, not an acoustic measurement.
class AudioTimestampObserver
{
public:
    struct Observation {
        bool sourceValid = false;
        bool phaseValid = false;
        std::int64_t sourceUs = -1;
        std::int64_t sourceGapUs = 0;
        std::int64_t videoUs = -1;
        std::int64_t estimatedLeadUs = 0;
        std::uint64_t discontinuities = 0;
    };
    Observation observe(std::int64_t sourceUs, int frames, int sampleRate,
                        std::uint32_t nowMs, int queuedMs, int deviceMs,
                        std::int64_t resamplerDelayUs,
                        const VideoClockSample& video, bool commonEpoch);
    void reset();
private:
    std::int64_t m_ExpectedSourceUs = -1;
    std::uint64_t m_Discontinuities = 0;
};

class AudioRateController
{
public:
    static constexpr int MaximumCorrectionPpm = 250;

    struct Result
    {
        int correctionPpm = 0;
        bool updated = false;
    };

    void reset();

    Result update(std::uint64_t rawAudioFrames,
                  std::uint64_t submittedAudioFrames,
                  int sampleRate,
                  std::uint32_t audioObservationTicks,
                  const VideoClockSample& videoClock);

    int correctionPpm() const;

private:
    struct ClockPoint
    {
        double clockMs;
        double mediaMs;
    };

    static double fitRate(const std::deque<ClockPoint>& points);
    static void trimWindow(std::deque<ClockPoint>& points);

    std::deque<ClockPoint> m_AudioPoints;
    std::deque<ClockPoint> m_VideoPoints;
    std::uint64_t m_FirstAudioFrames = 0;
    std::uint64_t m_FirstSubmittedAudioFrames = 0;
    std::int64_t m_FirstVideoMediaMs = 0;
    std::uint32_t m_FirstAudioObservation = 0;
    std::uint32_t m_FirstVideoPresentation = 0;
    std::uint32_t m_LastAudioObservation = 0;
    std::uint32_t m_LastVideoPresentation = 0;
    int m_CorrectionPpm = 0;
    bool m_Anchored = false;
};

// A snapshot at SDL's output pull boundary, not the speaker presentation clock.
// The renderer serializes this observer with SDL's stream lock. No allocation,
// logging or additional mutex is permitted on the output callback.
class AudioPlaybackObserver
{
public:
    struct Observation {
        std::uint64_t pulls = 0;
        std::uint64_t shortageRequests = 0;
        std::uint64_t missingInputBytes = 0;
        std::uint32_t ticks = 0;
        int requestUs = 0;
        int headroomUs = -1;
    };
    void observePull(int queuedBytes, int additionalBytes, int requestedBytes,
                     int bytesPerSecond, std::uint32_t ticks);
    Observation read() const { return m_Observation; }
private:
    Observation m_Observation;
};

// Common-epoch hosts only. Positive correction consumes source audio faster;
// negative correction slows it. Never combine this with the sample-rate fit
// used for hosts whose audio/video epochs are independent.
class AudioPhaseController
{
public:
    static constexpr int MaximumCorrectionPpm = 10000;

    struct Result
    {
        int correctionPpm = 0;
        bool updated = false;
    };

    void reset();

    Result update(const AudioTimestampObserver::Observation& timing,
                  int queuedAudioMs, std::uint32_t observationTicks,
                  const AudioPlaybackObserver::Observation& playback,
                  int sourceBlockUs);

    int correctionPpm() const;
    bool accelerationBlocked() const { return m_AccelerationBlocked; }

private:
    std::uint32_t m_LastUpdateTicks = 0;
    std::uint32_t m_LastObservationTicks = 0;
    double m_FilteredLeadUs = 0.0;
    int m_CorrectionPpm = 0;
    bool m_Anchored = false;
    std::uint64_t m_LastOutputPull = 0;
    std::uint64_t m_LastShortageRequests = 0;
    int m_HealthyOutputPulls = 0;
    bool m_AccelerationBlocked = true;
};

} // namespace PlankAvSync
