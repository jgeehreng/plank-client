#include "sdl.h"

#include <Limelight.h>
#include <cmath>

#if defined(HAVE_FFMPEG) && (defined(Q_OS_LINUX) || defined(Q_OS_MACOS))
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#endif

SdlAudioRenderer::SdlAudioRenderer(bool enableAvSyncCorrection, bool commonAudioVideoEpoch)
    : m_AudioStream(nullptr),
      m_AudioBuffer(nullptr),
      m_FrameSize(0),
      m_FrameDurationMs(0),
      m_BytesPerSampleFrame(0),
      m_BytesPerSecond(0),
      m_DeviceBufferDurationMs(0),
      m_SampleRate(0),
      m_ChannelCount(0),
      m_EnableAvSyncCorrection(enableAvSyncCorrection),
      m_RawAudioFrames(0),
      m_SubmittedAudioFrames(0),
      m_LastSubmittedAudioMediaTimeMs(-1),
      m_CommonAudioVideoEpoch(commonAudioVideoEpoch)
#if defined(HAVE_FFMPEG) && (defined(Q_OS_LINUX) || defined(Q_OS_MACOS))
      , m_SwrContext(nullptr)
#endif
{
    SDL_assert(!SDL_WasInit(SDL_INIT_AUDIO));

#ifdef Q_OS_LINUX
    // The qualified PLANK Linux client talks to PipeWire directly.
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "pipewire", SDL_HINT_OVERRIDE);
#endif

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s",
                     SDL_GetError());
        SDL_assert(SDL_WasInit(SDL_INIT_AUDIO));
    }
}

bool SdlAudioRenderer::prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION* opusConfig)
{
    SDL_AudioSpec want = {};

    want.freq = opusConfig->sampleRate;
    want.format = SDL_AUDIO_F32;
    want.channels = opusConfig->channelCount;

    m_FrameDurationMs = opusConfig->samplesPerFrame / (opusConfig->sampleRate / 1000);
    m_FrameSize = opusConfig->samplesPerFrame *
                  opusConfig->channelCount *
                  getAudioBufferSampleSize();

    m_AudioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                              &want, nullptr, nullptr);
    if (m_AudioStream == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to open audio device: %s",
                     SDL_GetError());
        return false;
    }

    SDL_AudioSpec deviceSpec = {};
    int deviceSampleFrames = 0;
    const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(m_AudioStream);
    if (!SDL_GetAudioDeviceFormat(device, &deviceSpec, &deviceSampleFrames)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to query audio device format: %s", SDL_GetError());
        return false;
    }

    // SDL3's device stream converts from this source format to the physical
    // device format. Queue accounting remains in source-format bytes.
    m_BytesPerSecond = want.freq * want.channels * getAudioBufferSampleSize();
    m_BytesPerSampleFrame = want.channels * getAudioBufferSampleSize();
    m_DeviceBufferDurationMs = deviceSampleFrames * 1000 / deviceSpec.freq;
    m_SampleRate = want.freq;
    m_ChannelCount = want.channels;

#if defined(HAVE_FFMPEG) && (defined(Q_OS_LINUX) || defined(Q_OS_MACOS))
    if (m_EnableAvSyncCorrection) {
        AVChannelLayout channelLayout;
        av_channel_layout_default(&channelLayout, want.channels);
        const int allocationResult = swr_alloc_set_opts2(
            &m_SwrContext,
            &channelLayout,
            AV_SAMPLE_FMT_FLT,
            want.freq,
            &channelLayout,
            AV_SAMPLE_FMT_FLT,
            opusConfig->sampleRate,
            0,
            nullptr);
        if (m_SwrContext != nullptr) {
            // Keep the resampler active from the first block. Otherwise the
            // first compensation request would reinitialize it mid-stream.
            av_opt_set_int(m_SwrContext, "flags", SWR_FLAG_RESAMPLE, 0);
        }
        av_channel_layout_uninit(&channelLayout);
        if (allocationResult < 0 || m_SwrContext == nullptr ||
                swr_init(m_SwrContext) < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to initialize PLANK A/V audio correction");
            swr_free(&m_SwrContext);
            m_EnableAvSyncCorrection = false;
        }
        else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "PLANK audio correction enabled: mode=%s limit=%d ppm",
                        m_CommonAudioVideoEpoch ? "source-phase" : "relative-rate",
                        m_CommonAudioVideoEpoch ?
                            PlankAvSync::AudioPhaseController::MaximumCorrectionPpm :
                            PlankAvSync::AudioRateController::MaximumCorrectionPpm);
        }
    }
#else
    m_EnableAvSyncCorrection = false;
#endif

    m_AudioBuffer = SDL_malloc(m_FrameSize);
    if (m_AudioBuffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to allocate audio buffer");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Decoded audio block: %u samples (%u bytes)",
                opusConfig->samplesPerFrame,
                m_FrameSize);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Audio device buffer: %u samples (%u ms)",
                deviceSampleFrames,
                m_DeviceBufferDurationMs);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "SDL audio driver: %s",
                SDL_GetCurrentAudioDriver());
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PLANK A/V source timing begin: common=%d observation_only=%d",
                m_CommonAudioVideoEpoch,
                !(m_EnableAvSyncCorrection && m_CommonAudioVideoEpoch));

    // Start playback
    // Observe demand without supplying, dropping or delaying any samples.
    // SDL holds the stream lock; snapshot reads use that same lock.
    if (!SDL_SetAudioStreamGetCallback(m_AudioStream, observeOutputPull, this)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to observe audio output: %s", SDL_GetError());
        return false;
    }
    if (!SDL_ResumeAudioStreamDevice(m_AudioStream)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to start audio stream: %s", SDL_GetError());
        return false;
    }

    return true;
}

SdlAudioRenderer::~SdlAudioRenderer()
{
    if (m_AudioStream != nullptr) {
        // Stop playback
        SDL_PauseAudioStreamDevice(m_AudioStream);
        SDL_DestroyAudioStream(m_AudioStream);
    }

    if (m_AudioBuffer != nullptr) {
        SDL_free(m_AudioBuffer);
    }

#if defined(HAVE_FFMPEG) && (defined(Q_OS_LINUX) || defined(Q_OS_MACOS))
    swr_free(&m_SwrContext);
#endif

    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    SDL_assert(!SDL_WasInit(SDL_INIT_AUDIO));
}

void SDLCALL SdlAudioRenderer::observeOutputPull(void* userdata, SDL_AudioStream* stream,
                                              int additionalBytes, int requestedBytes)
{
    auto* self = static_cast<SdlAudioRenderer*>(userdata);
    self->m_PlaybackObserver.observePull(SDL_GetAudioStreamQueued(stream),
        additionalBytes, requestedBytes, self->m_BytesPerSecond, SDL_GetTicks());
}

void* SdlAudioRenderer::getAudioBuffer(int*)
{
    return m_AudioBuffer;
}

bool SdlAudioRenderer::submitAudio(int bytesWritten, qint64 sourceTimeUs)
{
    if (bytesWritten == 0) {
        // Nothing to do
        return true;
    }

    const int inputFrames = m_BytesPerSampleFrame == 0 ?
                                0 : bytesWritten / m_BytesPerSampleFrame;

    // Provide backpressure on the queue to ensure too many frames don't build up
    // in SDL's audio queue, but don't wait forever to avoid a deadlock if the
    // audio device fails.
    for (int i = 0; i < 100; i++) {
        // Our device may enter a permanent error status upon removal, so we need
        // to recreate the audio device to pick up the new default audio device.
        if (SDL_GetAudioStreamDevice(m_AudioStream) == 0) {
            return false;
        }

        // Only queue more samples where there is 50 ms or less in SDL's queue
        if (SDL_GetAudioStreamQueued(m_AudioStream) / m_FrameSize * m_FrameDurationMs <= 50) {
            break;
        }

        SDL_Delay(1);
    }

    const void* queuedBuffer = m_AudioBuffer;
    int queuedBytes = bytesWritten;
    qint64 resamplerDelayUs = 0;

#if defined(HAVE_FFMPEG) && (defined(Q_OS_LINUX) || defined(Q_OS_MACOS))
    if (m_SwrContext != nullptr)
        resamplerDelayUs = swr_get_delay(m_SwrContext, 1000000);
#endif
    // One observation supplies both control and diagnostics at the same
    // pre-conversion/pre-enqueue boundary; do not count source gaps twice.
    SDL_LockAudioStream(m_AudioStream);
    const Uint64 observedAt = SDL_GetTicks();
    const int queuedMs = getQueuedAudioDurationMs();
    const auto playback = m_PlaybackObserver.read();
    SDL_UnlockAudioStream(m_AudioStream);
    const auto videoClock = PlankAvSync::readVideoClock();
    const auto timing = m_AudioTimestampObserver.observe(
        sourceTimeUs, inputFrames, m_SampleRate, static_cast<Uint32>(observedAt),
        queuedMs, m_DeviceBufferDurationMs, resamplerDelayUs,
        videoClock, m_CommonAudioVideoEpoch);

#if defined(HAVE_FFMPEG) && (defined(Q_OS_LINUX) || defined(Q_OS_MACOS))
    if (m_EnableAvSyncCorrection && m_SwrContext != nullptr && inputFrames > 0) {
        int appliedCorrectionPpm;
        bool correctionUpdated;
        if (m_CommonAudioVideoEpoch) {
            const auto correction = m_AudioPhaseController.update(
                timing, queuedMs, static_cast<Uint32>(observedAt), playback,
                inputFrames * 1000000LL / m_SampleRate);
            appliedCorrectionPpm = correction.correctionPpm;
            correctionUpdated = correction.updated;
        }
        else {
            // Linux Host clocks have independent epochs. Preserve its existing
            // relative-rate policy; never invent an absolute phase or combine
            // both controllers on the same stream.
            const auto correction = m_AudioRateController.update(
                m_RawAudioFrames, m_SubmittedAudioFrames, m_SampleRate,
                static_cast<Uint32>(observedAt), videoClock);
            appliedCorrectionPpm = correction.correctionPpm;
            correctionUpdated = correction.updated;
        }
        // Refresh the one-second compensation before it expires, including
        // the unchanged Linux relative-rate policy between its 1Hz updates.
        if (correctionUpdated || observedAt - m_LastCompensationUpdate >= 100) {
            m_LastCompensationUpdate = observedAt;
            constexpr int CompensationSeconds = 1;
            const int compensationDistance = m_SampleRate * CompensationSeconds;
            const int sampleDelta = static_cast<int>(std::llround(
                -static_cast<double>(appliedCorrectionPpm) *
                compensationDistance / 1000000.0));
            if (swr_set_compensation(m_SwrContext,
                                     sampleDelta,
                                     compensationDistance) < 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Unable to update PLANK audio correction");
            }
            else if (observedAt - m_LastCorrectionTelemetry >= 10000) {
                m_LastCorrectionTelemetry = observedAt;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "PLANK A/V audio correction: mode=%s applied=%d delta=%d distance=%d",
                            m_CommonAudioVideoEpoch ? "source-phase" : "relative-rate",
                            appliedCorrectionPpm,
                            sampleDelta,
                            compensationDistance);
            }
        }

        const int outputCapacity = swr_get_out_samples(m_SwrContext, inputFrames);
        m_CorrectedAudioBuffer.resize(
            static_cast<std::size_t>(outputCapacity) * m_ChannelCount);
        const uint8_t* inputPlanes[] = {
            reinterpret_cast<const uint8_t*>(m_AudioBuffer)
        };
        uint8_t* outputPlanes[] = {
            reinterpret_cast<uint8_t*>(m_CorrectedAudioBuffer.data())
        };
        const int outputFrames = swr_convert(m_SwrContext,
                                             outputPlanes,
                                             outputCapacity,
                                             inputPlanes,
                                             inputFrames);
        if (outputFrames < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PLANK audio correction failed");
            return false;
        }
        queuedBuffer = m_CorrectedAudioBuffer.data();
        queuedBytes = outputFrames * m_BytesPerSampleFrame;
    }
#endif

    m_RawAudioFrames += inputFrames;
    if (m_LastTimestampTelemetry == 0 || observedAt - m_LastTimestampTelemetry >= 1000) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PLANK A/V source timing: observe_ms=%llu common=%d valid=%d audio_us=%lld video_us=%lld estimated_lead_us=%lld queue_ms=%d device_ms=%d resampler_us=%lld gaps=%llu gap_us=%lld correction_ppm=%d",
                    static_cast<unsigned long long>(observedAt), m_CommonAudioVideoEpoch, timing.phaseValid,
                    static_cast<long long>(timing.sourceUs), static_cast<long long>(timing.videoUs),
                    static_cast<long long>(timing.estimatedLeadUs), queuedMs, m_DeviceBufferDurationMs,
                    static_cast<long long>(resamplerDelayUs),
                    static_cast<unsigned long long>(timing.discontinuities),
                    static_cast<long long>(timing.sourceGapUs), getAudioClockCorrectionPpm());
        m_LastTimestampTelemetry = observedAt;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PLANK audio output demand: pulls=%llu shortage_requests=%llu missing_input_bytes=%llu request_us=%d headroom_us=%d pull_age_ms=%u acceleration_blocked=%d",
                    static_cast<unsigned long long>(playback.pulls),
                    static_cast<unsigned long long>(playback.shortageRequests),
                    static_cast<unsigned long long>(playback.missingInputBytes),
                    playback.requestUs, playback.headroomUs,
                    static_cast<Uint32>(observedAt) - playback.ticks,
                    m_CommonAudioVideoEpoch && m_AudioPhaseController.accelerationBlocked());
    }
    if (!SDL_PutAudioStreamData(m_AudioStream, queuedBuffer, queuedBytes)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to queue audio sample: %s",
                     SDL_GetError());
    }
    else if (m_EnableAvSyncCorrection) {
        m_LastSubmittedAudioMediaTimeMs =
            m_SubmittedAudioFrames * 1000 / m_SampleRate;
        m_SubmittedAudioFrames += queuedBytes / m_BytesPerSampleFrame;
    }

    return true;
}

int SdlAudioRenderer::getCapabilities()
{
    // Keep audio submission on its worker: SDL output backpressure can wait.
    return CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION;
}

int SdlAudioRenderer::getQueuedAudioDurationMs()
{
    if (m_AudioStream == nullptr || m_BytesPerSecond == 0) {
        return -1;
    }

    const int queued = SDL_GetAudioStreamQueued(m_AudioStream);
    return queued < 0 ? -1 : static_cast<int>(queued * 1000ULL / m_BytesPerSecond);
}

int SdlAudioRenderer::getDeviceBufferDurationMs()
{
    return m_DeviceBufferDurationMs;
}

qint64 SdlAudioRenderer::getSubmittedAudioMediaTimeMs()
{
    return m_LastSubmittedAudioMediaTimeMs;
}

int SdlAudioRenderer::getAudioClockCorrectionPpm()
{
    return m_CommonAudioVideoEpoch ? m_AudioPhaseController.correctionPpm() :
                                    m_AudioRateController.correctionPpm();
}
IAudioRenderer::AudioFormat SdlAudioRenderer::getAudioBufferFormat()
{
    return AudioFormat::Float32NE;
}
