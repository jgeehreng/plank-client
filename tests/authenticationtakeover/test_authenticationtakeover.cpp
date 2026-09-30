#include "../../app/backend/authenticationtakeover.h"
#include <QtTest>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QFile>
#include <future>

class AuthenticationTakeoverTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        // Match main.cpp; the product never uses native macOS controls.
        QQuickStyle::setStyle("Material");
    }

    void strictConflict()
    {
        const QString id = QStringLiteral("11111111-2222-4333-8444-555555555555");
        QJsonObject reply{{"state", "conflict"}, {"error", "session_active"}, {"session_id", id}};
        QCOMPARE(macActiveSessionId(409, reply), id);
        QVERIFY(macActiveSessionId(403, reply).isEmpty());
        for (const QJsonValue& invalid : {QJsonValue(), QJsonValue(true), QJsonValue(42),
                QJsonValue("invalid"), QJsonValue("00000000-0000-0000-0000-000000000000"),
                QJsonValue("{" + id + "}")}) {
            reply["session_id"] = invalid;
            QVERIFY(macActiveSessionId(409, reply).isEmpty());
        }
        reply["session_id"] = id;
        reply["error"] = "session_changed";
        QVERIFY(macActiveSessionId(409, reply).isEmpty());
        reply["error"] = "session_active";
        reply["state"] = "denied";
        QVERIFY(macActiveSessionId(409, reply).isEmpty());
    }

    void decisionLifetime()
    {
        const auto accepted = AuthenticationTakeover::create();
        accepted->respond(true);
        accepted->respond(false);
        QVERIFY(accepted->wait(0));
        const auto cancelled = AuthenticationTakeover::create();
        cancelled->respond(false);
        cancelled->respond(true);
        QVERIFY(!cancelled->wait(0));
        const auto expired = AuthenticationTakeover::create();
        QVERIFY(!expired->wait(1));
        expired->respond(true);
        QVERIFY(!expired->wait(0));
    }

    void responsiveConsent_data()
    {
        QTest::addColumn<QString>("buttonText");
        QTest::addColumn<QString>("componentName");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("take-over") << QString("Take Over") << QString("SessionTakeoverDialog") << true;
        QTest::newRow("cancel") << QString("Cancel") << QString("SessionTakeoverDialog") << false;
        QTest::newRow("escape") << QString() << QString("SessionTakeoverDialog") << false;
        QTest::newRow("trust-replacement") << QString("Trust Replacement Host") << QString("HostTrustDialog") << true;
        QTest::newRow("trust-cancel") << QString("Cancel") << QString("HostTrustDialog") << false;
        QTest::newRow("trust-escape") << QString() << QString("HostTrustDialog") << false;
        QTest::newRow("trust-default-return") << QString("Return") << QString("HostTrustDialog") << false;
    }

    void responsiveConsent()
    {
        QFETCH(QString, buttonText);
        QFETCH(QString, componentName);
        QFETCH(bool, accepted);
        QQmlEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        QQmlComponent component(&engine);
        QByteArray qml = R"(
            import QtQuick 2.15
            import QtQuick.Controls 2.15
            import "qrc:/gui"
            ApplicationWindow {
                width: 900; height: 600; visible: true
                property int ticks: 0
                property int choice: -1
                Item { id: stackView }
                Timer { interval: 10; running: true; repeat: true; onTriggered: ticks++ }
                SessionTakeoverDialog {
                    objectName: "takeover"
                    onAccepted: choice = 1
                    onRejected: choice = 0
                }
            }
        )";
        qml.replace("SessionTakeoverDialog", componentName.toUtf8());
        component.setData(qml, QUrl("qrc:/takeover-test.qml"));
        QScopedPointer<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow*>(root.data());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        auto *dialog = root->findChild<QObject*>("takeover");
        QVERIFY(dialog);
        QVERIFY(dialog->property("modal").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        const auto decision = AuthenticationTakeover::create();
        auto waiting = std::async(std::launch::async, [decision] { return decision->wait(5000); });
        const int ticks = root->property("ticks").toInt();
        QTRY_VERIFY(root->property("ticks").toInt() >= ticks + 3);
        QVERIFY(waiting.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
        if (buttonText.isEmpty()) QTest::keyClick(window, Qt::Key_Escape);
        else if (buttonText == "Return") QTest::keyClick(window, Qt::Key_Return);
        else {
            auto *footer = qvariant_cast<QObject*>(dialog->property("footer"));
            QVERIFY(footer);
            QQuickItem *button = nullptr;
            for (auto *item : footer->findChildren<QQuickItem*>()) {
                if (item->property("text").toString() == buttonText &&
                        item->metaObject()->indexOfSignal("clicked()") >= 0) { button = item; break; }
            }
            QVERIFY(button);
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                button->mapToScene(QPointF(button->width()/2, button->height()/2)).toPoint());
        }
        QTRY_COMPARE(root->property("choice").toInt(), accepted ? 1 : 0);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        decision->respond(root->property("choice").toInt() == 1);
        QVERIFY(waiting.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        QCOMPARE(waiting.get(), accepted);
        QCOMPARE(warnings.count(), 0);
    }

    void rememberedUsernameDialog()
    {
        // Exercise the actual inline login dialog without loading the bookmark
        // page's network/streaming model. Only the model's two methods are fake.
        QFile source(QStringLiteral(":/gui/PcView.qml"));
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QByteArray page = source.readAll();
        const int start = page.indexOf("    NavigableDialog {\n        id: loginDialog");
        const int end = page.indexOf("    NavigableDialog {\n        id: editBookmarkDialog", start);
        QVERIFY(start >= 0 && end > start);
        QByteArray qml = R"(
            import QtQuick 2.15
            import QtQuick.Controls 2.15
            import QtQuick.Layouts 1.15
            import "qrc:/gui"
            ApplicationWindow {
                width: 900; height: 600; visible: true
                property alias dialog: loginDialog
                property alias username: usernameField
                property alias password: passwordField
                property string savedUsername: ""
                property string submittedUsername: ""
                property int attempts: 0
                Item { id: stackView }
                PlankTheme { id: theme }
                QtObject {
                    id: computerModel
                    function rememberedUsername(index) { return savedUsername }
                    function authenticateComputer(index, username, password) {
                        submittedUsername = username
                        attempts++
                    }
                }
        )" + page.mid(start, end - start) + "\n}";
        QQmlEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        QQmlComponent component(&engine);
        component.setData(qml, QUrl("qrc:/username-test.qml"));
        QScopedPointer<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto* window = qobject_cast<QQuickWindow*>(root.data());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        auto* dialog = root->property("dialog").value<QObject*>();
        auto* username = root->property("username").value<QObject*>();
        auto* password = root->property("password").value<QObject*>();
        QVERIFY(dialog && username && password);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(username->property("activeFocus").toBool());
        QVERIFY(username->property("text").toString().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        root->setProperty("savedUsername", "example-user");
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(password->property("activeFocus").toBool());
        QCOMPARE(username->property("text").toString(), QStringLiteral("example-user"));
        QCOMPARE(root->property("attempts").toInt(), 0); // no automatic login
        password->setProperty("text", "synthetic-only");
        QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QVERIFY(password->property("text").toString().isEmpty());
        QVERIFY(username->property("text").toString().isEmpty());
        QCOMPARE(root->property("attempts").toInt(), 0);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(password->property("activeFocus").toBool());
        username->setProperty("text", "different-user"); // prefill remains editable
        password->setProperty("text", "synthetic-only");
        QVERIFY(QMetaObject::invokeMethod(dialog, "startSignIn"));
        QTRY_COMPARE(root->property("attempts").toInt(), 1);
        QCOMPARE(root->property("submittedUsername").toString(), QStringLiteral("different-user"));
        QVERIFY(dialog->property("signingIn").toBool());
        QVERIFY(dialog->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QVERIFY(password->property("text").toString().isEmpty());
        QCOMPARE(warnings.count(), 0);
    }
};

QTEST_MAIN(AuthenticationTakeoverTest)
#include "test_authenticationtakeover.moc"
