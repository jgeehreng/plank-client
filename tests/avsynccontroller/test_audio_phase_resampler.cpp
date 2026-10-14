#include "avsynccontroller.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

// Exercise the actual FFmpeg compensation sign, rounding and output lengths.
// A simulated device clock consumes converted samples; its elapsed time drives
// video. This is not a real PipeWire/device or optical/acoustic measurement.
static void run(double initialLeadUs, double deviceErrorPpm)
{
    AVChannelLayout channels = AV_CHANNEL_LAYOUT_STEREO;
    SwrContext* swr = nullptr;
    assert(swr_alloc_set_opts2(&swr, &channels, AV_SAMPLE_FMT_FLT, 48000,
                             &channels, AV_SAMPLE_FMT_FLT, 48000, 0, nullptr) == 0);
    assert(av_opt_set_int(swr, "flags", SWR_FLAG_RESAMPLE, 0) == 0);
    assert(swr_init(swr) == 0);
    PlankAvSync::AudioTimestampObserver observer;
    PlankAvSync::AudioPhaseController controller;
    std::vector<float> input(240 * 2, 0.1f);
    std::vector<float> output;
    std::int64_t submitted = 0;
    double maxSettledUs = 0;
    double leadUs = 0;
    for (int block = 0; block < 120000; ++block) { // ten minutes at 5ms/block
        const double wallUs = submitted * 1000000.0 / 48000 /
                              (1.0 + deviceErrorPpm / 1000000.0);
        const auto now = static_cast<std::uint32_t>(wallUs / 1000) + 1000;
        constexpr std::int64_t EpochMs = 88890000;
        const auto sourceUs = static_cast<std::int64_t>(
            EpochMs * 1000 + block * 5000LL + 56000 + initialLeadUs);
        const auto delayUs = swr_get_delay(swr, 1000000);
        const auto timing = observer.observe(sourceUs / 1000 * 1000, 240, 48000,
            now, 35, 21, delayUs, {EpochMs + now - 1000, now, true}, true);
        assert(timing.phaseValid);
        PlankAvSync::AudioPlaybackObserver::Observation playback;
        playback.pulls = block + 1;
        playback.ticks = now;
        playback.requestUs = 21000;
        playback.headroomUs = 14000;
        const auto correction = controller.update(timing, 35, now, playback, 5000);
        if (correction.updated) {
            const int delta = static_cast<int>(std::llround(
                -correction.correctionPpm * 48000.0 / 1000000.0));
            assert(swr_set_compensation(swr, delta, 48000) == 0);
        }
        const int capacity = swr_get_out_samples(swr, 240);
        assert(capacity > 0 && capacity < 1024);
        output.resize(capacity * 2);
        const uint8_t* in[] = {reinterpret_cast<const uint8_t*>(input.data())};
        uint8_t* out[] = {reinterpret_cast<uint8_t*>(output.data())};
        const int frames = swr_convert(swr, out, capacity, in, 240);
        assert(frames > 0 && frames <= capacity);
        submitted += frames;
        leadUs = timing.estimatedLeadUs;
        if (block > 36000)
            maxSettledUs = std::max(maxSettledUs, std::abs(leadUs));
    }
    std::cout << "phase_resampler: initial_us=" << initialLeadUs
              << " device_ppm=" << deviceErrorPpm << " final_us=" << leadUs
              << " max_settled_us=" << maxSettledUs << std::endl;
    assert(maxSettledUs < 15000);
    swr_free(&swr);
}

int main()
{
    run(-350000, -400);
    run(350000, 400);
    std::cout << "audio_phase_resampler=pass simulated_device=1\n";
}
