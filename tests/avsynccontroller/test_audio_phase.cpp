#include "avsynccontroller.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <climits>
#include <iostream>

using namespace PlankAvSync;

// Idealized demand for the clock-only tests below. The SDL fixture separately
// exercises independent producer/device clocks and a finite, changing queue.
class TestController : public AudioPhaseController
{
public:
    Result update(const AudioTimestampObserver::Observation& timing, int queueMs,
                  std::uint32_t now)
    {
        AudioPlaybackObserver::Observation output;
        output.pulls = ++pulls;
        output.ticks = now;
        output.requestUs = 21000;
        output.headroomUs = std::max(0, queueMs - 21) * 1000;
        return AudioPhaseController::update(timing, queueMs, now, output, 5000);
    }
private:
    std::uint64_t pulls = 0;
};

static AudioTimestampObserver::Observation phase(std::int64_t leadUs)
{
    AudioTimestampObserver::Observation timing;
    timing.sourceValid = timing.phaseValid = true;
    timing.estimatedLeadUs = leadUs;
    return timing;
}

static void fill(TestController& controller, std::int64_t leadUs,
                 std::uint32_t start = 0)
{
    for (std::uint32_t ms = 0; ms <= 2000; ms += 5)
        controller.update(phase(leadUs), 35, start + ms);
}

static void safetyTests()
{
    TestController controller;
    fill(controller, 0);
    assert(controller.correctionPpm() == 0);
    controller.reset();
    fill(controller, -350000);
    assert(controller.correctionPpm() == 10000); // late: consume faster
    auto result = controller.update(phase(-350000), 9, 2005);
    assert(result.updated && result.correctionPpm == 0); // don't underrun
    controller.reset();
    fill(controller, 350000);
    assert(controller.correctionPpm() == -10000); // early: consume slower
    result = controller.update(phase(350000), 51, 2005);
    assert(result.updated && result.correctionPpm == 0); // don't build latency

    // Invalid timing/stale video/independent epochs must cancel an active rate.
    for (int reason = 0; reason < 8; ++reason) {
        controller.reset();
        fill(controller, -350000);
        auto timing = phase(-350000);
        int queueMs = 35;
        switch (reason) {
        case 0: timing.phaseValid = false; break;
        case 1: timing.sourceValid = false; break;
        case 2: timing.sourceGapUs = 100001; break;
        case 3: timing.sourceGapUs = -100001; break;
        case 4: timing.estimatedLeadUs = INT64_MAX; break;
        case 5: timing.estimatedLeadUs = INT64_MIN; break;
        case 6: queueMs = -1; break;
        case 7: queueMs = 2001; break;
        }
        result = controller.update(timing, queueMs, 2005);
        assert(result.updated && result.correctionPpm == 0);
        result = controller.update(phase(-10000), 35, 2010);
        assert(!result.updated && result.correctionPpm == 0);
    }
    controller.reset();
    fill(controller, -350000);
    result = controller.update(phase(-350000), 35, 5001);
    assert(result.updated && result.correctionPpm == 0); // stalled callback
    controller.reset();
    fill(controller, -350000, UINT32_MAX - 500);
    assert(controller.correctionPpm() == 10000); // local tick wrap
    controller.reset();
    assert(controller.correctionPpm() == 0); // device/reconnect lifetime

    // Output guards also prevent starting a rate, not just continuing it.
    for (std::uint32_t ms = 0; ms <= 2000; ms += 5)
        assert(controller.update(phase(-350000), 0, ms).correctionPpm <= 0);
    controller.reset();
    for (std::uint32_t ms = 0; ms <= 2000; ms += 5)
        assert(controller.update(phase(350000), 55, ms).correctionPpm == 0);
}

// Closed-loop source-clock model, not a speaker/SDL hardware acceptance test.
// Includes output latency, millisecond wire quantization, 60Hz video, unrelated
// local/Host epochs, jitter and positive/negative device-rate error. No frame
// counts are supplied to the controller, so callback throughput cannot bias it.
static void soak(double initialLeadUs, double deviceErrorPpm, bool jitter)
{
    AudioTimestampObserver observer;
    TestController controller;
    double leadUs = initialLeadUs;
    double maximumAfterSettling = 0;
    constexpr std::int64_t EpochMs = 88890000;
    int previousCorrection = 0;
    for (std::uint32_t ms = 0; ms <= 7200000; ms += 5) {
        const int jitterUs = jitter ? static_cast<int>((ms / 5) % 5) * 4000 - 8000 : 0;
        const auto frameMs = static_cast<std::uint32_t>((ms * 60ULL / 1000) * 1000 / 60);
        const auto now = ms + 1000;
        const VideoClockSample video {EpochMs + frameMs, frameMs + 1000, true};
        const auto sourceUs = static_cast<std::int64_t>(
            (EpochMs + ms) * 1000LL + 35000 + 21000 + 350 + leadUs + jitterUs);
        const auto timing = observer.observe(sourceUs / 1000 * 1000,
            240, 48000, now, 35, 21, 350, video, true);
        assert(timing.phaseValid);
        const auto result = controller.update(timing, 35, now);
        assert(std::abs(result.correctionPpm) <= AudioPhaseController::MaximumCorrectionPpm);
        if (result.updated)
            assert(std::abs(result.correctionPpm - previousCorrection) <= 1000);
        previousCorrection = result.correctionPpm;
        leadUs += (deviceErrorPpm + result.correctionPpm) * 0.005;
        if (ms >= 120000)
            maximumAfterSettling = std::max(maximumAfterSettling, std::abs(leadUs));
    }
    std::cout << "phase_soak: initial_us=" << initialLeadUs
              << " device_ppm=" << deviceErrorPpm << " jitter=" << jitter
              << " final_us=" << leadUs << " max_settled_us=" << maximumAfterSettling
              << " correction=" << controller.correctionPpm() << std::endl;
    assert(maximumAfterSettling < 16000);
    assert(std::abs(controller.correctionPpm() + deviceErrorPpm) < 100);
}

int main()
{
    safetyTests();
    soak(-350000, -250, false); // observed direction/size, must NOT slow further
    soak(350000, 250, false);
    soak(0, 0, false);
    soak(-350000, -400, true);
    soak(350000, 400, true);
    std::cout << "audio_phase_controller=pass synthetic_only=1\n";
}
