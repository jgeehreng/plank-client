import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import ComputerManager 1.0

Item {
    id: signInView
    objectName: qsTr("Sign In")
    anchors.fill: parent

    PlankTheme {
        id: theme
    }

    readonly property bool brokerAvailable: typeof plankBrokerSignInUrl !== "undefined" &&
                                             plankBrokerSignInUrl.length > 0

    // "idle": nothing attempted yet. "waiting": browser opened, polling for a
    // completed sign-in. "error": the last poll failed for a reason other
    // than a missing/expired session (e.g. the broker is unreachable).
    property string status: "idle"
    property string errorText: ""

    function startSignIn() {
        errorText = ""
        status = "waiting"
        if (brokerAvailable) {
            Qt.openUrlExternally(plankBrokerSignInUrl)
        }
        ComputerManager.refreshBrokerWorkstations()
        pollTimer.restart()
    }

    function handleWorkstationsReady() {
        pollTimer.stop()
        stackView.push("qrc:/gui/PcView.qml")
    }

    function handleSignInRequired() {
        // Still waiting on the browser-based sign-in; keep polling quietly.
        status = "waiting"
        errorText = ""
    }

    function handleRefreshFailed(error) {
        status = "error"
        errorText = error
    }

    StackView.onActivated: {
        ComputerManager.computerStateChanged.connect(handleWorkstationsReady)
        ComputerManager.brokerSignInRequired.connect(handleSignInRequired)
        ComputerManager.brokerRefreshFailed.connect(handleRefreshFailed)
    }

    StackView.onDeactivating: {
        pollTimer.stop()
        ComputerManager.computerStateChanged.disconnect(handleWorkstationsReady)
        ComputerManager.brokerSignInRequired.disconnect(handleSignInRequired)
        ComputerManager.brokerRefreshFailed.disconnect(handleRefreshFailed)
    }

    Timer {
        id: pollTimer
        interval: 2000
        repeat: true
        onTriggered: ComputerManager.refreshBrokerWorkstations()
    }

    ColumnLayout {
        anchors.centerIn: parent
        spacing: theme.spaceLarge
        width: Math.min(420, parent.width - 2 * theme.spaceLarge)

        Label {
            text: qsTr("Sign in required")
            font.pointSize: 22
            font.weight: Font.DemiBold
            color: theme.textPrimary
            Layout.alignment: Qt.AlignHCenter
        }

        Label {
            text: qsTr("Sign in with your facility account to see the workstations you're authorized to access.")
            wrapMode: Text.Wrap
            color: theme.textSecondary
            horizontalAlignment: Text.AlignHCenter
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
        }

        Button {
            text: signInView.status === "waiting" ? qsTr("Open sign-in page again") : qsTr("Sign in with browser")
            Layout.alignment: Qt.AlignHCenter
            enabled: signInView.brokerAvailable
            onClicked: signInView.startSignIn()
        }

        RowLayout {
            visible: signInView.status === "waiting"
            Layout.alignment: Qt.AlignHCenter
            spacing: theme.spaceSmall

            BusyIndicator {
                running: signInView.status === "waiting"
            }

            Label {
                text: qsTr("Waiting for sign-in to finish in your browser…")
                color: theme.textSecondary
                wrapMode: Text.Wrap
            }
        }

        Label {
            visible: signInView.status === "error"
            text: qsTr("Couldn't reach the PLANK broker. Retrying… (%1)").arg(signInView.errorText)
            color: theme.danger
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
        }

        Label {
            visible: !signInView.brokerAvailable
            text: qsTr("No PLANK broker is configured on this computer.")
            color: theme.danger
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
        }
    }
}
