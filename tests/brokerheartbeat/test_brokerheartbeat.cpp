// Plain C++ test of the Uppercut broker heartbeat policy (no Qt, no network).
#include "../../app/backend/brokerheartbeatpolicy.h"

#include <cstdio>
#include <cstdlib>

using Action = BrokerHeartbeatPolicy::Action;

static int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__,      \
                         __LINE__, #condition);                              \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static const char* first = "55555555-5555-4555-8555-555555555555";
static const char* second = "66666666-6666-4666-8666-666666666666";

static void noReservationSendsNothing()
{
    BrokerHeartbeatPolicy policy;
    CHECK(!policy.shouldSend(""));
    CHECK(policy.shouldSend(first));
}

static void healthyBeatsStayQuiet()
{
    BrokerHeartbeatPolicy policy;
    CHECK(policy.record(first, 200) == Action::Quiet);
    CHECK(policy.record(first, 200) == Action::Quiet);
    CHECK(policy.shouldSend(first));
}

static void failuresLogOncePerStreakAndRecover()
{
    BrokerHeartbeatPolicy policy;
    CHECK(policy.record(first, 0) == Action::LogFailure);
    CHECK(policy.record(first, 0) == Action::Quiet);
    CHECK(policy.record(first, 503) == Action::LogFailure); // a different failure
    CHECK(policy.record(first, 503) == Action::Quiet);
    CHECK(policy.shouldSend(first));                         // keeps trying
    CHECK(policy.record(first, 200) == Action::LogRecovered);
    CHECK(policy.record(first, 200) == Action::Quiet);
}

static void expiredSignInKeepsTrying()
{
    BrokerHeartbeatPolicy policy;
    CHECK(policy.record(first, 401) == Action::LogFailure);
    CHECK(policy.shouldSend(first));
}

static void endedReservationStopsUntilANewOne()
{
    BrokerHeartbeatPolicy policy;
    CHECK(policy.record(first, 200) == Action::Quiet);
    CHECK(policy.record(first, 404) == Action::Stop);
    CHECK(!policy.shouldSend(first));
    // A reconnect signs in again and holds a new reservation.
    CHECK(policy.shouldSend(second));
    CHECK(policy.record(second, 200) == Action::Quiet);
    CHECK(policy.shouldSend(second));
}

static void foreignReservationStops()
{
    BrokerHeartbeatPolicy policy;
    CHECK(policy.record(first, 403) == Action::Stop);
    CHECK(!policy.shouldSend(first));
}

static void intervalLeavesRoomInsideTheGracePeriod()
{
    // The broker frees a reservation 120 s after the last heartbeat; three
    // missed beats must still fit.
    CHECK(BrokerHeartbeatPolicy::IntervalMs * 3 < 120000);
}

int main()
{
    noReservationSendsNothing();
    healthyBeatsStayQuiet();
    failuresLogOncePerStreakAndRecover();
    expiredSignInKeepsTrying();
    endedReservationStopsUntilANewOne();
    foreignReservationStops();
    intervalLeavesRoomInsideTheGracePeriod();
    if (failures != 0) {
        std::fprintf(stderr, "broker heartbeat policy: %d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("broker heartbeat policy: pass");
    return EXIT_SUCCESS;
}
