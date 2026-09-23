#include <QtTest>

#include "linuxrawwacom.h"

class TestWacomTransportPolicy : public QObject
{
    Q_OBJECT

private slots:
    void usesNormalizedPenForFirstGenerationIntuosPro();
    void usesExactRawHidForNewerWacoms();
    void doesNotApplyWacomFallbackToAnotherVendor();
    void treatsALateHidrawInterfaceAsAnIncompleteGroup();
};

void TestWacomTransportPolicy::usesNormalizedPenForFirstGenerationIntuosPro()
{
    QCOMPARE(plankWacomTransportForUsbDevice(0x056a, 0x0314),
             PlankWacomTransport::NormalizedPen);
    QCOMPARE(plankWacomTransportForUsbDevice(0x056a, 0x0315),
             PlankWacomTransport::NormalizedPen);
    QCOMPARE(plankWacomTransportForUsbDevice(0x056a, 0x0317),
             PlankWacomTransport::NormalizedPen);
}

void TestWacomTransportPolicy::usesExactRawHidForNewerWacoms()
{
    QCOMPARE(plankWacomTransportForUsbDevice(0x056a, 0x0357),
             PlankWacomTransport::ExactRawHid);
    QCOMPARE(plankWacomTransportForUsbDevice(0x056a, 0x0358),
             PlankWacomTransport::ExactRawHid);
    QCOMPARE(plankWacomTransportForUsbDevice(0x056a, 0x0400),
             PlankWacomTransport::ExactRawHid);
}

void TestWacomTransportPolicy::doesNotApplyWacomFallbackToAnotherVendor()
{
    QCOMPARE(plankWacomTransportForUsbDevice(0x1234, 0x0315),
             PlankWacomTransport::ExactRawHid);
}

void TestWacomTransportPolicy::treatsALateHidrawInterfaceAsAnIncompleteGroup()
{
    const std::vector<std::string> touchOnly = {"/dev/hidraw2"};
    const std::vector<std::string> penAndTouch = {
        "/dev/hidraw1", "/dev/hidraw2"};
    QVERIFY(!plankRawWacomGroupIncomplete({}, penAndTouch));
    QVERIFY(!plankRawWacomGroupIncomplete(penAndTouch, touchOnly));
    QVERIFY(!plankRawWacomGroupIncomplete(penAndTouch, penAndTouch));
    QVERIFY(plankRawWacomGroupIncomplete(touchOnly, penAndTouch));
}

QTEST_APPLESS_MAIN(TestWacomTransportPolicy)
#include "test_wacomtransportpolicy.moc"
