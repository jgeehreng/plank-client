#pragma once

#include <string>

// Uppercut: what one broker heartbeat result means for the next one.
//
// The broker keeps a workstation reserved while the client's stream runs and
// frees it two minutes after the last heartbeat. A failed heartbeat never
// interrupts the stream; the Host already admitted it. Plain C++ so it can be
// tested without Qt or a network.
class BrokerHeartbeatPolicy
{
public:
    enum class Action {
        Quiet,        // nothing worth logging
        LogFailure,   // first failure of a streak, or the failure changed
        LogRecovered, // a heartbeat succeeded after failures
        Stop,         // this reservation is gone or not ours; stop beating it
    };

    // Interval between heartbeats. The broker's grace period is 120 seconds.
    static constexpr int IntervalMs = 30000;

    // Whether to send a heartbeat for this reservation now.
    bool shouldSend(const std::string& admissionId) const
    {
        return !admissionId.empty() && !(m_Stopped && admissionId == m_AdmissionId);
    }

    // status is the broker's HTTP status, or 0 when it did not answer.
    Action record(const std::string& admissionId, int status)
    {
        if (admissionId != m_AdmissionId) {
            // A reconnect signed in again and holds a new reservation.
            m_AdmissionId = admissionId;
            m_Stopped = false;
            m_Failing = false;
            m_LastStatus = -1;
        }
        if (status == 200) {
            const bool recovered = m_Failing;
            m_Failing = false;
            m_LastStatus = status;
            return recovered ? Action::LogRecovered : Action::Quiet;
        }
        if (status == 404 || status == 403) {
            // 404: released or expired. 403: another subject's reservation.
            // Neither recovers, so stop until the reservation changes.
            m_Stopped = true;
            m_LastStatus = status;
            return Action::Stop;
        }
        // 401 (facility sign-in expired), 5xx and no answer may recover.
        const bool changed = !m_Failing || status != m_LastStatus;
        m_Failing = true;
        m_LastStatus = status;
        return changed ? Action::LogFailure : Action::Quiet;
    }

private:
    std::string m_AdmissionId;
    bool m_Stopped = false;
    bool m_Failing = false;
    int m_LastStatus = -1;
};
