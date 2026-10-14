#include <QtTest>

#include "avsynccontroller.h"

#include <cmath>

class TestAvSyncController : public QObject
{
    Q_OBJECT

private slots:
    void leavesMatchedClocksUnchanged();
    void correctsFastAudioClock();
    void correctsSlowAudioClock();
    void boundsExtremeCorrection();
    void ignoresStaleVideoClock();
    void boundsLongRunPhaseError();
};

namespace {
int runClockModel(double audioPpm, int durationSeconds)
{
    PlankAvSync::AudioRateController controller;
    double submittedFrames = 0.0;
    std::uint64_t previousRawFrames = 0;
    for (int second = 0; second <= durationSeconds; second++) {
        const auto audioFrames = static_cast<std::uint64_t>(std::llround(
            second * 48000.0 * (1.0 + audioPpm / 1000000.0)));
        if (second != 0) {
            submittedFrames += (audioFrames - previousRawFrames) *
                (1.0 - controller.correctionPpm() / 1000000.0);
        }
        previousRawFrames = audioFrames;
        const PlankAvSync::VideoClockSample video {
            second * 1000LL,
            static_cast<std::uint32_t>(second * 1000),
            true
        };
        controller.update(audioFrames,
                          static_cast<std::uint64_t>(std::llround(submittedFrames)),
                          48000,
                          static_cast<std::uint32_t>(second * 1000 + 20),
                          video);
    }
    return controller.correctionPpm();
}
}

void TestAvSyncController::leavesMatchedClocksUnchanged()
{
    QCOMPARE(runClockModel(0.0, 180), 0);
}

void TestAvSyncController::correctsFastAudioClock()
{
    const int correction = runClockModel(30.0, 180);
    QVERIFY(correction >= 28);
    QVERIFY(correction <= 32);
}

void TestAvSyncController::correctsSlowAudioClock()
{
    const int correction = runClockModel(-45.0, 180);
    QVERIFY(correction >= -50);
    QVERIFY(correction <= -43);
}

void TestAvSyncController::boundsExtremeCorrection()
{
    QCOMPARE(runClockModel(1000.0, 300),
             PlankAvSync::AudioRateController::MaximumCorrectionPpm);
    QCOMPARE(runClockModel(-1000.0, 300),
             -PlankAvSync::AudioRateController::MaximumCorrectionPpm);
}

void TestAvSyncController::ignoresStaleVideoClock()
{
    PlankAvSync::AudioRateController controller;
    const PlankAvSync::VideoClockSample current {0, 1000, true};
    const auto initial = controller.update(0, 0, 48000, 1020, current);
    QVERIFY(!initial.updated);

    const PlankAvSync::VideoClockSample stale {1000, 2000, true};
    const auto ignored = controller.update(48000, 48000, 48000, 5001, stale);
    QVERIFY(!ignored.updated);
    QCOMPARE(ignored.correctionPpm, 0);
}

void TestAvSyncController::boundsLongRunPhaseError()
{
    PlankAvSync::AudioRateController controller;
    constexpr double AudioPpm = 80.0;
    constexpr double VideoPpm = 3.0;
    double submittedFrames = 0.0;
    std::uint64_t previousRawFrames = 0;
    double relativePhaseErrorMs = 0.0;
    for (int second = 0; second <= 7200; second++) {
        const auto rawFrames = static_cast<std::uint64_t>(std::llround(
            second * 48000.0 * (1.0 + AudioPpm / 1000000.0)));
        if (second != 0) {
            submittedFrames += (rawFrames - previousRawFrames) *
                (1.0 - controller.correctionPpm() / 1000000.0);
        }
        previousRawFrames = rawFrames;
        const auto submitted = static_cast<std::uint64_t>(
            std::llround(submittedFrames));
        const auto videoMediaMs = static_cast<std::int64_t>(std::llround(
            second * 1000.0 * (1.0 + VideoPpm / 1000000.0)));
        controller.update(rawFrames,
                          submitted,
                          48000,
                          static_cast<std::uint32_t>(second * 1000 + 20),
                          {videoMediaMs,
                           static_cast<std::uint32_t>(second * 1000),
                           true});
        relativePhaseErrorMs =
            second * 1000.0 - submitted * 1000.0 / 48000.0 -
            (second * 1000.0 - videoMediaMs);
    }
    QVERIFY(std::abs(relativePhaseErrorMs) < 20.0);
}

QTEST_APPLESS_MAIN(TestAvSyncController)
#include "test_avsynccontroller.moc"
