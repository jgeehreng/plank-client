#pragma once

#include "renderer.h"
#include "streaming/avsynccontroller.h"
#include <SDL3/SDL.h>
#include <vector>

#if defined(HAVE_FFMPEG) && (defined(Q_OS_LINUX) || defined(Q_OS_MACOS))
struct SwrContext;
#endif

class SdlAudioRenderer : public IAudioRenderer
{
public:
    explicit SdlAudioRenderer(bool enableAvSyncCorrection = false, bool commonAudioVideoEpoch = false);

    virtual ~SdlAudioRenderer();

    virtual bool prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION* opusConfig);

    virtual void* getAudioBuffer(int* size);

    virtual bool submitAudio(int bytesWritten, qint64 sourceTimeUs);

    virtual int getCapabilities();

    virtual int getQueuedAudioDurationMs() override;

    virtual int getDeviceBufferDurationMs() override;

    virtual qint64 getSubmittedAudioMediaTimeMs() override;

    virtual int getAudioClockCorrectionPpm() override;

    virtual AudioFormat getAudioBufferFormat();

private:
    static void SDLCALL observeOutputPull(void* userdata, SDL_AudioStream* stream,
                                         int additionalBytes, int requestedBytes);
    SDL_AudioStream* m_AudioStream;
    void* m_AudioBuffer;
    int m_FrameSize;
    int m_FrameDurationMs;
    int m_BytesPerSampleFrame;
    int m_BytesPerSecond;
    int m_DeviceBufferDurationMs;
    int m_SampleRate;
    int m_ChannelCount;
    bool m_EnableAvSyncCorrection;
    quint64 m_RawAudioFrames;
    quint64 m_SubmittedAudioFrames;
    qint64 m_LastSubmittedAudioMediaTimeMs;
    PlankAvSync::AudioRateController m_AudioRateController;
    PlankAvSync::AudioPhaseController m_AudioPhaseController;
    bool m_CommonAudioVideoEpoch;
    Uint64 m_LastTimestampTelemetry = 0;
    Uint64 m_LastCorrectionTelemetry = 0;
    Uint64 m_LastCompensationUpdate = 0;
    PlankAvSync::AudioTimestampObserver m_AudioTimestampObserver;
    PlankAvSync::AudioPlaybackObserver m_PlaybackObserver;

#if defined(HAVE_FFMPEG) && (defined(Q_OS_LINUX) || defined(Q_OS_MACOS))
    SwrContext* m_SwrContext;
    std::vector<float> m_CorrectedAudioBuffer;
#endif
};
