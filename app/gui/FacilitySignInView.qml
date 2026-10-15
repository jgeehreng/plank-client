import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import BrokerSignIn 1.0
import ComputerManager 1.0

// Uppercut: facility sign-in in the PCoIP client's shape. Saved connections,
// then username and password, then Duo (Send Me a Push), then
// the workstations this account may use.
Item {
    id: signInView
    objectName: page === "connections" ? qsTr("Saved connections") :
                page === "edit" ? (editIndex < 0 ? qsTr("Add connection") : qsTr("Edit connection")) :
                qsTr("Sign in")
    anchors.fill: parent

    PlankTheme {
        id: theme
    }

    // "connections", "edit" or "signin". The sign-in page follows
    // BrokerSignIn.step for the password and Duo steps.
    property string page: "connections"
    property int editIndex: -1
    property bool awaitingWorkstations: false
    property string listError: ""

    readonly property bool mfaStep: BrokerSignIn.step === "mfa" || BrokerSignIn.step === "sending_push" ||
                                    BrokerSignIn.step === "waiting_push" || BrokerSignIn.step === "checking_passcode"
    readonly property bool busy: BrokerSignIn.step === "signing_in" || BrokerSignIn.step === "sending_push" ||
                                 BrokerSignIn.step === "checking_passcode" || awaitingWorkstations

    function showConnections() {
        BrokerSignIn.signOut()
        listError = ""
        if (BrokerSignIn.connections.length === 0) {
            editConnection(-1)
        } else {
            page = "connections"
            editIndex = -1
        }
    }

    function openConnection(index) {
        BrokerSignIn.selectConnection(index)
        listError = ""
        page = "signin"
        usernameField.text = BrokerSignIn.rememberedUsername(index)
        rememberBox.checked = usernameField.text.length > 0
        passwordField.text = ""
        if (usernameField.text.length > 0)
            passwordField.forceActiveFocus()
        else
            usernameField.forceActiveFocus()
    }

    function editConnection(index) {
        editIndex = index
        var row = index >= 0 ? BrokerSignIn.connections[index] : null
        addressField.text = row ? row.address : ""
        nameField.text = row ? row.name : ""
        editError.text = ""
        page = "edit"
        addressField.forceActiveFocus()
    }

    function saveConnection() {
        var error = BrokerSignIn.saveConnection(editIndex, nameField.text, addressField.text)
        if (error.length > 0) {
            editError.text = error
            return
        }
        openConnection(editIndex < 0 ? BrokerSignIn.connections.length - 1 : editIndex)
    }

    function connect() {
        if (usernameField.text.length === 0 || passwordField.text.length === 0)
            return
        BrokerSignIn.signIn(usernameField.text, passwordField.text, rememberBox.checked)
        passwordField.text = ""
    }


    function handleSignedIn() {
        listError = ""
        awaitingWorkstations = true
        ComputerManager.refreshBrokerWorkstations()
    }

    function handleWorkstationsReady() {
        if (!awaitingWorkstations)
            return
        awaitingWorkstations = false
        stackView.push("qrc:/gui/PcView.qml")
    }

    function handleSignInRequired() {
        if (!awaitingWorkstations)
            return
        awaitingWorkstations = false
        BrokerSignIn.signOut()
        listError = qsTr("The broker didn't accept the new sign-in. Sign in again.")
    }

    function handleRefreshFailed(error) {
        if (!awaitingWorkstations)
            return
        awaitingWorkstations = false
        BrokerSignIn.signOut()
        listError = qsTr("Couldn't load your workstations: %1").arg(error)
    }

    Component.onCompleted: {
        if (BrokerSignIn.connections.length === 0)
            editConnection(-1)
        else if (BrokerSignIn.connections.length === 1)
            openConnection(0)
    }

    StackView.onActivated: {
        BrokerSignIn.signedIn.connect(handleSignedIn)
        ComputerManager.computerStateChanged.connect(handleWorkstationsReady)
        ComputerManager.brokerSignInRequired.connect(handleSignInRequired)
        ComputerManager.brokerRefreshFailed.connect(handleRefreshFailed)
        // Coming back from the workstation list ends that facility session,
        // as leaving a connection does in the PCoIP client.
        if (BrokerSignIn.step === "signed_in" && !awaitingWorkstations) {
            BrokerSignIn.signOut()
            if (BrokerSignIn.selectedConnection >= 0)
                openConnection(BrokerSignIn.selectedConnection)
        }
    }

    StackView.onDeactivating: {
        BrokerSignIn.signedIn.disconnect(handleSignedIn)
        ComputerManager.computerStateChanged.disconnect(handleWorkstationsReady)
        ComputerManager.brokerSignInRequired.disconnect(handleSignInRequired)
        ComputerManager.brokerRefreshFailed.disconnect(handleRefreshFailed)
    }

    Connections {
        target: BrokerSignIn
        function onStepChanged() {
            if (BrokerSignIn.step === "mfa" && BrokerSignIn.factors.indexOf("push") >= 0)
                pushButton.forceActiveFocus()
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Math.max(theme.spaceLarge, parent.width * 0.1)
        anchors.rightMargin: Math.max(theme.spaceLarge, parent.width * 0.1)
        anchors.topMargin: theme.spaceLarge * 2
        anchors.bottomMargin: theme.spaceLarge
        spacing: theme.spaceLarge * 2

        // Left: what this step is, and which connection it is for.
        ColumnLayout {
            Layout.fillHeight: true
            // A child with fillWidth would otherwise make this column fill
            // too and squeeze the form off the right edge.
            Layout.fillWidth: false
            Layout.preferredWidth: Math.max(220, Math.min(420, parent.width * 0.4))
            spacing: theme.spaceLarge

            Label {
                text: signInView.page === "connections" ? qsTr("Saved connections") :
                      signInView.page === "edit" ? (signInView.editIndex < 0 ? qsTr("Add connection")
                                                                            : qsTr("Edit connection")) :
                      signInView.mfaStep ? qsTr("Multi-factor authentication") : qsTr("Connect")
                font.pointSize: 26
                color: theme.textPrimary
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

            Item {
                visible: signInView.page === "signin"
                implicitHeight: connectionLabels.implicitHeight
                Layout.fillWidth: true
                Layout.topMargin: theme.spaceLarge * 2

                Rectangle {
                    width: 2
                    height: parent.height
                    color: theme.accent
                }

                Column {
                    id: connectionLabels
                    x: 14
                    spacing: 6

                    Label {
                        text: BrokerSignIn.connectionName
                        color: theme.textPrimary
                        font.pointSize: 14
                        font.weight: Font.DemiBold
                    }
                    Label {
                        visible: signInView.mfaStep
                        text: BrokerSignIn.username
                        color: theme.textSecondary
                        font.pointSize: 13
                    }
                }
            }

            Item { Layout.fillHeight: true }

            Rectangle {
                visible: signInView.page !== "connections" && BrokerSignIn.connections.length > 0
                height: 1
                color: theme.borderSubtle
                Layout.fillWidth: true
            }

            Label {
                visible: signInView.page !== "connections" && BrokerSignIn.connections.length > 0
                text: "←  " + qsTr("Saved connections")
                color: backArea.containsMouse ? theme.accent : theme.textPrimary
                font.pointSize: 14
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Saved connections")

                MouseArea {
                    id: backArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: signInView.showConnections()
                }
            }
        }

        // Right: the form for this step.
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 300

            // ---- Saved connections ----
            ColumnLayout {
                visible: signInView.page === "connections"
                width: Math.min(parent.width, 420)
                spacing: theme.spaceMedium

                Repeater {
                    model: BrokerSignIn.connections

                    delegate: ItemDelegate {
                        id: connectionRow
                        Layout.fillWidth: true
                        implicitHeight: 64
                        background: Rectangle {
                            radius: theme.radiusMedium
                            color: connectionRow.down ? theme.surfacePressed :
                                   (connectionRow.hovered ? theme.surfaceHover : theme.surface)
                        }
                        contentItem: RowLayout {
                            spacing: theme.spaceMedium
                            Column {
                                Layout.fillWidth: true
                                Label {
                                    text: modelData.name
                                    color: theme.textPrimary
                                    font.pointSize: 15
                                    font.weight: Font.DemiBold
                                }
                                Label {
                                    text: modelData.address
                                    color: theme.textSecondary
                                    font.pointSize: 10
                                }
                            }
                            Button {
                                text: qsTr("Edit")
                                flat: true
                                onClicked: signInView.editConnection(index)
                            }
                        }
                        onClicked: signInView.openConnection(index)
                    }
                }

                Button {
                    text: qsTr("Add connection")
                    flat: true
                    onClicked: signInView.editConnection(-1)
                }
            }

            // ---- Add or edit a connection ----
            ColumnLayout {
                visible: signInView.page === "edit"
                width: Math.min(parent.width, 420)
                spacing: theme.spaceSmall

                Label { text: qsTr("Host Address"); color: theme.textSecondary }
                PlankTextField {
                    id: addressField
                    placeholderText: qsTr("broker.example.com")
                    inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhUrlCharactersOnly
                    Layout.fillWidth: true
                    Keys.onReturnPressed: nameField.forceActiveFocus()
                }

                Label {
                    text: qsTr("Connection Name")
                    color: theme.textSecondary
                    Layout.topMargin: theme.spaceMedium
                }
                PlankTextField {
                    id: nameField
                    placeholderText: qsTr("New York")
                    Layout.fillWidth: true
                    Keys.onReturnPressed: signInView.saveConnection()
                    Keys.onEnterPressed: signInView.saveConnection()
                }

                Label {
                    id: editError
                    visible: text.length > 0
                    color: theme.danger
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                Button {
                    text: qsTr("Save")
                    highlighted: true
                    enabled: addressField.text.trim().length > 0
                    Layout.fillWidth: true
                    Layout.topMargin: theme.spaceLarge
                    onClicked: signInView.saveConnection()
                }
                Button {
                    text: qsTr("Cancel")
                    visible: BrokerSignIn.connections.length > 0
                    Layout.fillWidth: true
                    onClicked: signInView.showConnections()
                }
                Button {
                    text: qsTr("Delete connection")
                    flat: true
                    visible: signInView.editIndex >= 0
                    onClicked: {
                        BrokerSignIn.removeConnection(signInView.editIndex)
                        signInView.showConnections()
                    }
                }
            }

            // ---- Username and password ----
            ColumnLayout {
                visible: signInView.page === "signin" && !signInView.mfaStep
                width: Math.min(parent.width, 420)
                spacing: theme.spaceSmall

                Label { text: qsTr("Username"); color: theme.textSecondary }
                PlankTextField {
                    id: usernameField
                    enabled: !signInView.busy
                    inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    Layout.fillWidth: true
                    Keys.onReturnPressed: passwordField.forceActiveFocus()
                }

                Label {
                    text: qsTr("Password")
                    color: theme.textSecondary
                    Layout.topMargin: theme.spaceMedium
                }
                PlankTextField {
                    id: passwordField
                    enabled: !signInView.busy
                    echoMode: TextInput.Password
                    placeholderText: qsTr("Password")
                    Layout.fillWidth: true
                    Keys.onReturnPressed: signInView.connect()
                    Keys.onEnterPressed: signInView.connect()
                }

                Button {
                    text: signInView.busy ? qsTr("Connecting…") : qsTr("Connect")
                    highlighted: true
                    enabled: !signInView.busy && usernameField.text.length > 0 && passwordField.text.length > 0
                    Layout.fillWidth: true
                    Layout.topMargin: theme.spaceLarge
                    onClicked: signInView.connect()
                }

                BusyIndicator {
                    running: signInView.busy
                    visible: running
                    Layout.alignment: Qt.AlignHCenter
                }

                PlankCheckBox {
                    id: rememberBox
                    visible: BrokerSignIn.rememberUsernameAllowed
                    text: qsTr("Remember my username")
                    Layout.topMargin: theme.spaceLarge
                }

                Label {
                    visible: text.length > 0
                    text: signInView.listError.length > 0 ? signInView.listError : BrokerSignIn.errorText
                    color: theme.danger
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
            }

            // ---- Duo: Send Me a Push ----
            ColumnLayout {
                visible: signInView.page === "signin" && signInView.mfaStep
                width: Math.min(parent.width, 420)
                spacing: theme.spaceSmall

                Label {
                    text: qsTr("Approve the sign-in with Duo on your phone or watch.")
                    color: theme.textSecondary
                    wrapMode: Text.Wrap
                    visible: BrokerSignIn.factors.indexOf("push") >= 0 && BrokerSignIn.step !== "waiting_push"
                    Layout.fillWidth: true
                    Layout.bottomMargin: theme.spaceMedium
                }

                Label {
                    text: qsTr("Your Duo account has no device that can receive a push. Ask IT to add Duo Mobile to your account.")
                    color: theme.danger
                    wrapMode: Text.Wrap
                    visible: BrokerSignIn.factors.indexOf("push") < 0
                    Layout.fillWidth: true
                }

                Button {
                    id: pushButton
                    visible: BrokerSignIn.factors.indexOf("push") >= 0 && BrokerSignIn.step !== "waiting_push"
                    text: BrokerSignIn.step === "sending_push" ? qsTr("Sending…") : qsTr("Send Me a Push")
                    highlighted: true
                    enabled: !signInView.busy
                    Layout.fillWidth: true
                    onClicked: BrokerSignIn.sendPush()
                }

                ColumnLayout {
                    visible: BrokerSignIn.step === "waiting_push"
                    spacing: theme.spaceSmall
                    Layout.fillWidth: true

                    RowLayout {
                        spacing: theme.spaceSmall
                        Layout.fillWidth: true
                        BusyIndicator { running: BrokerSignIn.step === "waiting_push" }
                        Label {
                            text: qsTr("Push sent. Approve it on your phone or watch.")
                            color: theme.textPrimary
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }
                    }
                    Label {
                        visible: BrokerSignIn.verificationCode.length > 0
                        text: qsTr("Enter %1 in Duo Mobile.").arg(BrokerSignIn.verificationCode)
                        color: theme.textPrimary
                        font.pointSize: 16
                        font.weight: Font.DemiBold
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            text: qsTr("Send another push")
                            flat: true
                            onClicked: BrokerSignIn.sendPush()
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            text: qsTr("Cancel")
                            onClicked: {
                                BrokerSignIn.cancel()
                                passwordField.forceActiveFocus()
                            }
                        }
                    }
                }

                Label {
                    visible: text.length > 0
                    text: BrokerSignIn.errorText
                    color: theme.danger
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    Layout.topMargin: theme.spaceMedium
                }
            }
        }
    }
}
