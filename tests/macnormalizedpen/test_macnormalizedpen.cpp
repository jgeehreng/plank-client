#include <QtTest>
#include "macnormalizedpenlogic.h"

class TestMacNormalizedPen : public QObject {
    Q_OBJECT
private slots:
    void clampsUnitInterval();
    void mapsButtonsAndTool();
    void mapsMotionAndMouseId();
};

void TestMacNormalizedPen::clampsUnitInterval()
{
    QCOMPARE(MacNormalizedPenLogic::clamp01(-0.25f), 0.0f);
    QCOMPARE(MacNormalizedPenLogic::clamp01(0.4f), 0.4f);
    QCOMPARE(MacNormalizedPenLogic::clamp01(1.8f), 1.0f);
}

void TestMacNormalizedPen::mapsButtonsAndTool()
{
    using namespace MacNormalizedPenLogic;
    QCOMPARE(buttons(0), static_cast<unsigned char>(0));
    QCOMPARE(buttons(kInputButton1 | kInputButton3),
             static_cast<unsigned char>(kButtonPrimary | kButtonTertiary));
    QCOMPARE(buttonMask(1), kButtonPrimary);
    QCOMPARE(buttonMask(2), kButtonSecondary);
    QCOMPARE(buttonMask(3), kButtonTertiary);
    QCOMPARE(buttonMask(4), static_cast<unsigned char>(0));
    QCOMPARE(tool(0), kToolPen);
    QCOMPARE(tool(kInputEraser), kToolEraser);
}

void TestMacNormalizedPen::mapsMotionAndMouseId()
{
    using namespace MacNormalizedPenLogic;
    QCOMPARE(motionEvent(false), kHover);
    QCOMPARE(motionEvent(true), kMove);
    QVERIFY(isPenMouse(static_cast<unsigned>(-2)));
    QVERIFY(!isPenMouse(0));
}

QTEST_MAIN(TestMacNormalizedPen)
#include "test_macnormalizedpen.moc"
