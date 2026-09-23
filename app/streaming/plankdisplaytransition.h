#pragma once

#include <cstdint>

// A display-layout change stops the host media worker and brings it back on
// the requested mode. Another /launch while that change is in flight submits
// a new one, so the first connection never settles. Wait for the layout, and
// only then launch. If the layout is still wrong after the host has had time
// to finish, submit the change once more.
class PlankDisplayTransitionLaunch
{
public:
    static constexpr std::uint64_t RetryAfterMs = 4000;

    enum class Action {
        Wait,
        Launch,
    };

    void noteSubmitted(std::uint64_t nowMs)
    {
        m_InFlight = true;
        m_RetryAtMs = nowMs + RetryAfterMs;
    }

    Action decide(bool layoutMatches, bool workerReachable, std::uint64_t nowMs)
    {
        if (!workerReachable) {
            return Action::Wait;
        }
        if (layoutMatches) {
            m_InFlight = false;
            m_RetryAtMs = 0;
            return Action::Launch;
        }
        if (m_InFlight && nowMs < m_RetryAtMs) {
            return Action::Wait;
        }
        if (m_InFlight) {
            m_RetryAtMs = nowMs + RetryAfterMs;
        }
        return Action::Launch;
    }

private:
    bool m_InFlight = false;
    std::uint64_t m_RetryAtMs = 0;
};
