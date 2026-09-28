#include "microphone.h"
#include <SDL3/SDL.h>
#include "microphoneopus.h"
#include <QDebug>
#include <chrono>
#include <memory>
#ifdef PLANK_TIMED_MICROPHONE
#include "linuxmicrophone.h"
#endif
#ifdef Q_OS_MACOS
#include "macmicrophonepermission.h"
#endif
#ifdef PLANK_TRANSPORT
#include <plank_transport.h>
#include <plank_transport_control.h>
#endif

PlankMicrophone::PlankMicrophone(PlankTransportNativeEndpoint* endpoint,
                               std::atomic<bool>& requested, bool automaticInput, bool timed)
    : m_Endpoint(endpoint), m_Requested(requested), m_AutomaticInput(automaticInput), m_Timed(timed),
      m_Thread(&PlankMicrophone::run, this)
{
}

PlankMicrophone::~PlankMicrophone()
{
    {
        std::lock_guard<std::mutex> guard(m_Mutex);
        m_Stop = true;
    }
    m_Wake.notify_all();
    m_Thread.join();
}

void PlankMicrophone::acknowledge(std::uint64_t generation, std::uint32_t state)
{
    std::lock_guard<std::mutex> guard(m_Mutex);
    if (generation >= m_AckGeneration) {
        m_AckGeneration = generation;
        m_AckState = state;
    }
    m_Wake.notify_all();
}

void PlankMicrophone::run()
{
#ifndef PLANK_TRANSPORT
    m_State.store(State::Unavailable);
#else
    using Clock = std::chrono::steady_clock;
#ifdef PLANK_TIMED_MICROPHONE
    std::unique_ptr<PlankLinuxMicrophone> timedCapture;
    std::uint64_t timedNextSample = 0;
#endif
    SDL_AudioStream* stream = nullptr;
    OpusEncoder* encoder = nullptr;
    bool audioInitialized = false;
    const auto closeCapture = [&] {
#ifdef PLANK_TIMED_MICROPHONE
        timedCapture.reset();
#endif
        if (stream) SDL_DestroyAudioStream(stream);
        if (encoder) opus_encoder_destroy(encoder);
        if (audioInitialized) SDL_QuitSubSystem(SDL_INIT_AUDIO);
        stream = nullptr; encoder = nullptr; audioInitialized = false;
    };
    std::uint64_t command = 0, sampleTime = 0;
    bool configured = false, sentEnabled = false, failed = false;
    bool lastRequested = m_Requested.load();
    auto deadline = Clock::now();
    auto lastSamples = Clock::now();
    bool captured = false;
    for (;;) {
        std::uint64_t ackGeneration;
        std::uint32_t ackState;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            if (m_Wake.wait_for(lock, std::chrono::milliseconds(5), [&] { return m_Stop; })) break;
            ackGeneration = m_AckGeneration; ackState = m_AckState;
        }
        const bool requested = m_Requested.load();
        if (requested != lastRequested) { failed = false; lastRequested = requested; }
        int permission = 1;
#ifdef Q_OS_MACOS
        // Consent is requested by the launcher, never by a stream/toolbar toggle.
        // An unprovisioned CLI launch remains usable without microphone capture.
        if (requested && !failed) {
            permission = plankMacMicrophonePermission() == 1 ? 1 : -1;
            if (permission < 0) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Microphone permission is unavailable; authorize PLANK Client in System Settings or reopen the launcher before connecting");
        }
#endif
        if (permission < 0) failed = true;
        const bool enabled = requested && !failed && permission == 1;
        if (!enabled) closeCapture();
        if (!configured || enabled != sentEnabled) {
            // Stop locally before sending mute; a full reliable queue must never
            // leave capture running while waiting for the remote acknowledgement.
            closeCapture(); plank_transport_native_microphone_activate(m_Endpoint, 0);
            const std::uint64_t next = command + 1;
            const std::uint32_t words[] = {std::uint32_t(next >> 32), std::uint32_t(next),
                (enabled ? PLANK_TRANSPORT_MICROPHONE_ENABLED : 0u) |
                (m_AutomaticInput ? PLANK_TRANSPORT_MICROPHONE_AUTO_INPUT : 0u)};
            std::uint8_t packet[20]; size_t size = 0;
            plank_transport_control_encode(PLANK_TRANSPORT_CONTROL_SET_MICROPHONE, words, 3, packet, sizeof(packet), &size);
            if (plank_transport_native_data_send(m_Endpoint, packet, size) != PLANK_TRANSPORT_OK) {
                m_State.store(failed ? State::Unavailable : State::Pending); continue;
            }
            command = next; configured = true; sentEnabled = enabled;
            deadline = Clock::now() + std::chrono::seconds(5);
            sampleTime = 0;
            captured = false;
        }
        if (failed) { m_State.store(State::Unavailable); continue; }
        if (requested && !permission) { m_State.store(State::Pending); continue; }
        if (ackGeneration != command || ackState == PLANK_TRANSPORT_MICROPHONE_PENDING) {
            if (Clock::now() >= deadline) { failed = true; closeCapture(); }
            m_State.store(failed ? State::Unavailable : State::Pending); continue;
        }
        if (ackState == PLANK_TRANSPORT_MICROPHONE_UNAVAILABLE ||
            plank_transport_native_microphone_state(m_Endpoint) == 3) {
            failed = true; closeCapture(); m_State.store(State::Unavailable); continue;
        }
        if (!enabled) { m_State.store(State::Off); continue; }
        if (ackState != PLANK_TRANSPORT_MICROPHONE_ACTIVE) { failed = true; continue; }
#ifdef PLANK_TIMED_MICROPHONE
        if (m_Timed) {
            if (!timedCapture) {
                timedCapture.reset(new PlankLinuxMicrophone);
                timedNextSample = 0;
                encoder = plankMicrophoneCreateEncoder();
                if (!timedCapture->valid() || !encoder ||
                        plank_transport_native_microphone_activate(m_Endpoint, command) != PLANK_TRANSPORT_OK) {
                    failed = true; closeCapture(); continue;
                }
                lastSamples = Clock::now();
            }
            if (!timedCapture->valid()) { failed = true; closeCapture(); continue; }
            int lookahead = 0;
            if (opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&lookahead)) != OPUS_OK || lookahead < 0 || lookahead > 480) {
                failed = true; closeCapture(); continue;
            }
            PlankLinuxMicrophone::Packet input;
            for (unsigned i = 0; i < 6 && timedCapture->take(input); i++) {
                const auto codecDelay = std::uint64_t(lookahead) * 1000000000 / PlankMicrophoneRate;
                if (input.captureTimeNs <= codecDelay) { failed = true; break; }
                if (input.sampleTime != timedNextSample && opus_encoder_ctl(encoder, OPUS_RESET_STATE) != OPUS_OK) {
                    failed = true; break;
                }
                timedNextSample = input.sampleTime + 480;
                std::uint8_t packet[1275];
                const int bytes = opus_encode_float(encoder, input.samples, 480, packet, sizeof(packet));
                if (bytes < 1) { failed = true; break; }
                const int result = plank_transport_native_microphone_send_timed(m_Endpoint, command,
                    input.sampleTime, input.captureTimeNs - codecDelay, packet, std::size_t(bytes));
                captured = true; lastSamples = Clock::now();
                if (result != PLANK_TRANSPORT_OK && result != PLANK_TRANSPORT_DROPPED && result != PLANK_TRANSPORT_TIMEOUT) {
                    failed = true; break;
                }
            }
            if (Clock::now() - lastSamples > std::chrono::seconds(2)) failed = true;
            if (failed) closeCapture();
            m_State.store(failed ? State::Unavailable : captured ? State::Active : State::Pending);
            continue;
        }
#else
        if (m_Timed) { failed = true; closeCapture(); continue; }
#endif
        if (!stream) {
            audioInitialized = SDL_InitSubSystem(SDL_INIT_AUDIO);
            if (audioInitialized) {
                SDL_AudioSpec format {SDL_AUDIO_F32, PlankMicrophoneChannels, PlankMicrophoneRate};
                stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &format, nullptr, nullptr);
                encoder = plankMicrophoneCreateEncoder();
            }
            if (!stream || !encoder ||
                plank_transport_native_microphone_activate(m_Endpoint, command) != PLANK_TRANSPORT_OK ||
                !SDL_ResumeAudioStreamDevice(stream)) {
                qWarning() << "PLANK microphone capture is unavailable";
                failed = true; closeCapture(); continue;
            }
            lastSamples = Clock::now();
        }
        int available = SDL_GetAudioStreamAvailable(stream);
        if (available < 0) { failed = true; closeCapture(); continue; }
        const int frameBytes = PlankMicrophoneChannels * int(sizeof(float));
        const int packetBytes = PlankMicrophoneFrames * frameBytes;
        if (available > 2880 * frameBytes) {
            // Discard a stalled capture backlog, preserving a timestamp gap for
            // the Host. Never play seconds-old speech after a scheduling stall.
            sampleTime += std::uint64_t(available / packetBytes) * PlankMicrophoneFrames;
            SDL_ClearAudioStream(stream); continue;
        }
        for (int i = 0; i < 6 && available >= packetBytes; ++i) {
            float samples[PlankMicrophoneFrames * PlankMicrophoneChannels]; std::uint8_t packet[1275];
            if (SDL_GetAudioStreamData(stream, samples, sizeof(samples)) != int(sizeof(samples))) { failed = true; break; }
            const int bytes = opus_encode_float(encoder, samples, 480, packet, sizeof(packet));
            if (bytes < 1) { failed = true; break; }
            const int result = plank_transport_native_microphone_send(m_Endpoint, command, sampleTime, packet, size_t(bytes));
            sampleTime += 480;
            captured = true; lastSamples = Clock::now();
            if (result != PLANK_TRANSPORT_OK && result != PLANK_TRANSPORT_DROPPED &&
                result != PLANK_TRANSPORT_TIMEOUT) { failed = true; break; }
            available -= sizeof(samples);
        }
        if (Clock::now() - lastSamples > std::chrono::seconds(2)) failed = true;
        if (failed) closeCapture();
        m_State.store(failed ? State::Unavailable : captured ? State::Active : State::Pending);
    }
    closeCapture();
    plank_transport_native_microphone_activate(m_Endpoint, 0);
#endif
}
