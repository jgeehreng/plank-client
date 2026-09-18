import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import ComputerModel 1.0

import ComputerManager 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0

CenteredGridView {
    property ComputerModel computerModel : createModel()

    id: pcGrid

    PlankTheme {
        id: theme
    }

    focus: true
    activeFocusOnTab: true
    topMargin: 24
    bottomMargin: 24
    // A workstation is the primary object on this page, so let its row use
    // the complete content width instead of stopping at an arbitrary cap.
    cellWidth: availableWidth
    cellHeight: 88
    Component.onCompleted: {
        // Don't show any highlighted item until interacting with them.
        // We do this here instead of onActivated to avoid losing the user's
        // selection when backing out of a different page of the app.
        currentIndex = -1
    }

    // Note: Any initialization done here that is critical for streaming must
    // also be done in CliStartStreamSegue.qml, since this code does not run
    // for command-line initiated streams.
    StackView.onActivated: {
        // Setup signals on CM
        ComputerManager.computerAddCompleted.connect(addComplete)

    }

    StackView.onDeactivating: {
        ComputerManager.computerAddCompleted.disconnect(addComplete)
    }

    function authenticationComplete(error)
    {
        var pcIndex = loginDialog.pcIndex
        loginDialog.close()
        if (error !== undefined) {
            errorDialog.text = error
            errorDialog.open()
        } else {
            launchPlankDesktop(pcIndex)
        }
    }

    function launchPlankDesktop(pcIndex)
    {
        var session = computerModel.createSessionForPlankDesktop(pcIndex)
        if (session === null) {
            errorDialog.text = qsTr("The workstation did not provide its Desktop session.")
            errorDialog.open()
            return
        }

        var component = Qt.createComponent("StreamSegue.qml")
        var segue = component.createObject(stackView, {
                                               "appName": qsTr("Desktop"),
                                               "session": session,
                                               "isResume": false
                                           })
        stackView.push(segue)
    }

    function addComplete(success)
    {
        if (!success) {
            errorDialog.text = qsTr("Unable to connect to the specified PC.")

            errorDialog.open()
        }
    }

    function createModel()
    {
        var model = Qt.createQmlObject('import ComputerModel 1.0; ComputerModel {}', parent, '')
        model.initialize(ComputerManager)
        model.authenticationCompleted.connect(authenticationComplete)
        model.relayWakeCompleted.connect(function(error) {
            if (error !== undefined) {
                errorDialog.text = error
                errorDialog.open()
            }
        })
        return model
    }

    Row {
        anchors.centerIn: parent
        spacing: theme.spaceMedium
        visible: pcGrid.count === 0

        BusyIndicator {
            id: searchSpinner
            visible: StreamingPreferences.enableMdns
        }

        Label {
            height: searchSpinner.height
            elide: Label.ElideRight
            text: StreamingPreferences.enableMdns ? qsTr("Searching for compatible hosts on your local network...")
                                                  : qsTr("Automatic PC discovery is disabled. Add your PC manually.")
            color: theme.textSecondary
            font.pointSize: 15
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
        }
    }

    model: computerModel

    delegate: NavigableItemDelegate {
        id: pcEntry
        width: pcGrid.cellWidth
        height: 76
        grid: pcGrid
        Accessible.name: model.name
        hoverEnabled: true

        background: Rectangle {
            radius: theme.radiusMedium
            color: pcEntry.down ? theme.surfacePressed :
                   (pcEntry.hovered || pcEntry.highlighted ? theme.surfaceHover : theme.surface)
            border.width: pcEntry.highlighted ? 1 : 0
            border.color: theme.accent

            Behavior on color {
                ColorAnimation { duration: 90 }
            }
        }

        property alias pcContextMenu : pcContextMenuLoader.item

        Image {
            id: stateIcon
            anchors.left: parent.left
            anchors.leftMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            visible: !model.statusUnknown
            source: !model.online ? "qrc:/res/warning_FILL1_wght300_GRAD200_opsz24.svg" :
                                    (!model.authorized ? "qrc:/res/baseline-lock-24px.svg" :
                                                     "qrc:/res/baseline-check_circle_outline-24px.svg")
            sourceSize {
                width: 28
                height: 28
            }
        }

        BusyIndicator {
            id: statusUnknownSpinner
            anchors.horizontalCenter: stateIcon.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            width: 28
            height: 28
            visible: model.statusUnknown
        }

        Column {
            id: workstationIdentity
            anchors.left: stateIcon.right
            anchors.leftMargin: 14
            anchors.right: workstationStatus.left
            anchors.rightMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3

            Label {
                id: pcNameText
                width: parent.width
                text: model.name
                color: theme.textPrimary
                font.pointSize: 16
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }

            Label {
                width: parent.width
                text: model.address ? model.address : qsTr("Address unavailable")
                color: theme.textSecondary
                font.pointSize: 10
                elide: Text.ElideRight
            }
        }

        Column {
            id: workstationStatus
            width: 190
            anchors.right: parent.right
            anchors.rightMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3

            Label {
                width: parent.width
                text: model.statusUnknown ? qsTr("Checking") :
                      (model.online ? qsTr("Online") : qsTr("Offline"))
                color: model.statusUnknown ? theme.textSecondary :
                       (model.online ? theme.success : theme.textDisabled)
                font.pointSize: 11
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignRight
            }

            Label {
                width: parent.width
                text: model.plankHostVersion ?
                          model.plankHostVersion : " "
                color: theme.textSecondary
                font.pointSize: 9
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideLeft
            }
        }

        Loader {
            id: pcContextMenuLoader
            asynchronous: true
            sourceComponent: NavigableMenu {
                id: pcContextMenu
                MenuItem {
                    text: qsTr("PC Status: %1").arg(model.online ? qsTr("Online") : qsTr("Offline"))
                    font.bold: true
                    enabled: false
                }
                NavigableMenuItem {
                    parentMenu: pcContextMenu
                    text: qsTr("Wake PC")
                    visible: computerModel.relayWakeEnabled && model.manualBookmark &&
                             !model.statusUnknown && !model.online
                    onTriggered: computerModel.requestRelayWake(index)
                }
                NavigableMenuItem {
                    parentMenu: pcContextMenu
                    text: qsTr("Edit bookmark…")
                    visible: model.manualBookmark
                    onTriggered: {
                        editBookmarkDialog.pcIndex = index
                        editBookmarkDialog.originalAddress = model.address
                        editBookmarkDialog.originalNickname = model.name
                        editBookmarkDialog.scalingIndex =
                                computerModel.plankScalingChoice(index)
                        editBookmarkDialog.hostLayoutIndex =
                                computerModel.plankHostLayoutChoice(index)
                        editBookmarkDialog.virtualMode1Index =
                                computerModel.plankVirtualMode1Choice(index)
                        editBookmarkDialog.virtualMode2Index =
                                computerModel.plankVirtualMode2Choice(index)
                        editBookmarkDialog.originalProfile = computerModel.plankVideoProfile(index)
                        editBookmarkDialog.originalCaptureSource =
                                computerModel.plankCaptureSource(index)
                        editBookmarkDialog.originalProfileBitratesKbps =
                                computerModel.plankProfileBitratesKbps(index)
                        editBookmarkDialog.open()
                    }
                }
                NavigableMenuItem {
                    parentMenu: pcContextMenu
                    text: qsTr("Rename PC")
                    onTriggered: {
                        renamePcDialog.pcIndex = index
                        renamePcDialog.originalName = model.name
                        renamePcDialog.open()
                    }
                    visible: !model.manualBookmark
                }
                NavigableMenuItem {
                    parentMenu: pcContextMenu
                    text: qsTr("Delete PC")
                    onTriggered: {
                        deletePcDialog.pcIndex = index
                        deletePcDialog.pcName = model.name
                        deletePcDialog.open()
                    }
                }
            }
        }

        onClicked: {
            if (model.online) {
                if (model.authorized) {
                    launchPlankDesktop(index)
                }
                else {
                    loginDialog.pcIndex = index
                    loginDialog.open()
                }
            } else if (!model.online) {
                // Using open() here because it may be activated by keyboard
                pcContextMenu.open()
            }
        }

        onPressAndHold: {
            // popup() ensures the menu appears under the mouse cursor
            if (pcContextMenu.popup) {
                pcContextMenu.popup()
            }
            else {
                // Qt 5.9 doesn't have popup()
                pcContextMenu.open()
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton;
            onClicked: {
                parent.pressAndHold()
            }
        }

        Keys.onMenuPressed: {
            // We must use open() here so the menu is positioned on
            // the ItemDelegate and not where the mouse cursor is
            pcContextMenu.open()
        }

        Keys.onDeletePressed: {
            deletePcDialog.pcIndex = index
            deletePcDialog.pcName = model.name
            deletePcDialog.open()
        }
    }

    ErrorMessageDialog {
        id: errorDialog
    }

    NavigableDialog {
        id: loginDialog
        property int pcIndex: -1
        title: qsTr("Sign in to workstation")
        modal: true
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            usernameField.text = computerModel.plankUsername(pcIndex)
            passwordField.clear()
            if (usernameField.text) {
                passwordField.forceActiveFocus()
            } else {
                usernameField.forceActiveFocus()
            }
        }
        onClosed: {
            passwordField.clear()
        }
        onAccepted: {
            if (usernameField.text && passwordField.text) {
                computerModel.authenticateComputer(pcIndex, usernameField.text,
                                                   passwordField.text)
            }
        }

        ColumnLayout {
            spacing: 8

            Label {
                text: qsTr("Use your workstation operating-system account.")
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Label {
                text: qsTr("Username")
                color: theme.textSecondary
                Layout.fillWidth: true
            }
            PlankTextField {
                id: usernameField
                Layout.fillWidth: true
                focus: true
            }
            Label {
                text: qsTr("Password")
                color: theme.textSecondary
                Layout.fillWidth: true
                Layout.topMargin: 4
            }
            PlankTextField {
                id: passwordField
                echoMode: TextInput.Password
                Layout.fillWidth: true
                Keys.onReturnPressed: loginDialog.accept()
                Keys.onEnterPressed: loginDialog.accept()
            }
        }
    }

    NavigableDialog {
        id: editBookmarkDialog
        property int pcIndex: -1
        property string originalAddress: ""
        property string originalNickname: ""
        property int scalingIndex: 1
        property int hostLayoutIndex: 0
        property int hostDisplayPolicy: -1
        property int virtualMode1Index: 9
        property int virtualMode2Index: 1
        property var virtualModeChoices: ComputerManager.plankVirtualModeChoices()
        property int originalProfile: StreamingPreferences.PLANK_PROFILE_H264_10BIT_444
        property int originalCaptureSource: StreamingPreferences.PLANK_CAPTURE_NVFBC_8BIT
        property var originalProfileBitratesKbps: []
        property var profileBitratesKbps: []
        title: qsTr("Edit workstation bookmark")
        width: Math.min(640, parent.width - 40)
        height: Math.min(implicitHeight, parent.height - 20)
        dim: false
        modal: true
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.Ok | Dialog.Cancel

        function currentVideoProfile() {
            if (editEncodingProfile.currentIndex >= 0 &&
                    editEncodingProfile.currentIndex < editEncodingProfile.model.count) {
                return editEncodingProfile.model.get(
                            editEncodingProfile.currentIndex).val
            }
            return StreamingPreferences.PLANK_PROFILE_H264_10BIT_444
        }

        function applyProfileBitrate() {
            var profile = currentVideoProfile()
            var saved = profileBitratesKbps[profile]
            editBitrateSlider.value = saved === undefined ?
                        StreamingPreferences.plankDefaultBitrateKbps(profile) : saved
        }

        function rememberProfileBitrate() {
            var values = []
            for (var i = 0; i < profileBitratesKbps.length; ++i) {
                values.push(profileBitratesKbps[i])
            }
            values[currentVideoProfile()] = Math.round(editBitrateSlider.value)
            profileBitratesKbps = values
        }

        function virtualModeSupported(mode) {
            return StreamingPreferences.plankVirtualModeSupportedForProfile(
                        mode, currentVideoProfile())
        }

        function ensureVirtualModesCompatible() {
            if (editCaptureSource.captureSource === 2) return
            var fallback = -1
            for (var i = 0; i < virtualModeChoices.length; ++i) {
                if (virtualModeChoices[i] === "4096\u00d72160") {
                    fallback = i
                    break
                }
            }
            if (fallback < 0) {
                return
            }
            if (!virtualModeSupported(virtualModeChoices[editVirtualMode1.currentIndex])) {
                editVirtualMode1.currentIndex = fallback
            }
            if (!virtualModeSupported(virtualModeChoices[editVirtualMode2.currentIndex])) {
                editVirtualMode2.currentIndex = fallback
            }
        }

        onOpened: {
            editAddressText.text = originalAddress
            editNicknameText.text = originalNickname
            editScalingChoice.currentIndex = scalingIndex
            hostDisplayPolicy = computerModel.plankHostDisplayPolicy(pcIndex)
            editCaptureSource.selectCaptureSource(originalCaptureSource)
            editHostLayout.currentIndex = hostLayoutIndex
            editVirtualMode1.currentIndex = virtualMode1Index
            editVirtualMode2.currentIndex = virtualMode2Index
            for (var i = 0; i < editEncodingProfile.model.count; i++) {
                if (editEncodingProfile.model.get(i).val === originalProfile) {
                    editEncodingProfile.currentIndex = i
                    break
                }
            }
            var loadedBitrates = []
            for (var bitrateIndex = 0;
                 bitrateIndex < originalProfileBitratesKbps.length;
                 ++bitrateIndex) {
                loadedBitrates.push(originalProfileBitratesKbps[bitrateIndex])
            }
            profileBitratesKbps = loadedBitrates
            applyProfileBitrate()
            ensureVirtualModesCompatible()
            editAddressText.forceActiveFocus()
            standardButton(Dialog.Ok).enabled = Qt.binding(function() {
                return editAddressText.text.trim() !== "" &&
                       editNicknameText.text.trim() !== "" &&
                       editScalingChoice.currentIndex >= 0
            })
        }
        onClosed: {
            editAddressText.clear()
            editNicknameText.clear()
            hostDisplayPolicy = -1
            originalProfileBitratesKbps = []
            profileBitratesKbps = []
        }
        onAccepted: {
            if (!computerModel.editComputerBookmark(pcIndex,
                                                    editAddressText.text.trim(),
                                                    editNicknameText.text.trim(),
                                                    editScalingChoice.currentIndex,
                                                    editHostLayout.currentIndex,
                                                    editVirtualMode1.currentIndex,
                                                    editVirtualMode2.currentIndex,
                                                    editEncodingProfile.model.get(
                                                        editEncodingProfile.currentIndex).val,
                                                    editCaptureSource.captureSource,
                                                    profileBitratesKbps)) {
                errorDialog.text = qsTr("Unable to update the workstation bookmark. Check the address and ensure another bookmark is not already using it.")
                errorDialog.open()
            }
        }

        ColumnLayout {
            width: parent.width

            Label {
                text: qsTr("Address or hostname")
                font.bold: true
            }
            PlankTextField {
                id: editAddressText
                Layout.fillWidth: true
            }

            Label {
                text: qsTr("Nickname")
                font.bold: true
            }
            PlankTextField {
                id: editNicknameText
                Layout.fillWidth: true
            }

            Label {
                text: qsTr("Capture source")
                font.bold: true
            }
            PlankCaptureSourceBox {
                id: editCaptureSource
                Layout.fillWidth: true
                hostAddress: editAddressText.text
                probingEnabled: editBookmarkDialog.visible
                onCaptureSourceChanged: {
                    editHostLayout.currentIndex = 0
                    editEncodingProfile.currentIndex = captureSource === 0 ? 3 : 0
                    Qt.callLater(editBookmarkDialog.applyProfileBitrate)
                }
            }

            Label {
                text: qsTr("Encoding profile")
                font.bold: true
            }
            PlankComboBox {
                id: editEncodingProfile
                Layout.fillWidth: true
                textRole: "text"
                model: editCaptureSource.captureSource === 2 ? editAppleEncodingProfileModel :
                       editCaptureSource.captureSource === 0 ?
                           editNvfbcEncodingProfileModel : editNativeEncodingProfileModel
                onActivated: {
                    editBookmarkDialog.applyProfileBitrate()
                    editBookmarkDialog.ensureVirtualModesCompatible()
                }
            }

            ListModel {
                id: editNvfbcEncodingProfileModel
                ListElement {
                    text: qsTr("H.264 8-bit 4:2:2")
                    val: StreamingPreferences.PLANK_PROFILE_H264_8BIT_422
                }
                ListElement {
                    text: qsTr("H.264 8-bit 4:4:4 (identity GBR)")
                    val: StreamingPreferences.PLANK_PROFILE_H264_8BIT_444
                }
                ListElement {
                    text: qsTr("H.264 10-bit 4:2:2")
                    val: StreamingPreferences.PLANK_PROFILE_H264_10BIT_422
                }
                ListElement {
                    text: qsTr("H.264 10-bit 4:4:4 (identity GBR)")
                    val: StreamingPreferences.PLANK_PROFILE_H264_10BIT_444
                }
                ListElement {
                    text: qsTr("H.264 8-bit 4:4:4 (identity GBR) — NVENC")
                    val: StreamingPreferences.PLANK_PROFILE_NVENC_H264_8BIT_444
                }
                ListElement {
                    text: qsTr("H.265 8-bit 4:4:4 (identity GBR) — NVENC")
                    val: StreamingPreferences.PLANK_PROFILE_NVENC_HEVC_8BIT_444
                }
                ListElement {
                    text: qsTr("H.265 10-bit 4:4:4 (identity GBR) — NVENC")
                    val: StreamingPreferences.PLANK_PROFILE_NVENC_HEVC_10BIT_444
                }
            }

            ListModel {
                id: editAppleEncodingProfileModel
                ListElement {
                    text: qsTr("HEVC 10-bit 4:2:0 — Apple VideoToolbox (Preview)")
                    val: StreamingPreferences.PLANK_PROFILE_APPLE_HEVC_10BIT_420
                }
                ListElement {
                    text: qsTr("HEVC 10-bit 4:4:4 — Apple VideoToolbox (Preview)")
                    val: StreamingPreferences.PLANK_PROFILE_APPLE_HEVC_10BIT_444
                }
            }

            ListModel {
                id: editNativeEncodingProfileModel
                ListElement {
                    text: qsTr("H.264 10-bit 4:4:4 (identity GBR) — x264")
                    val: StreamingPreferences.PLANK_PROFILE_H264_10BIT_444
                }
                ListElement {
                    text: qsTr("H.265 10-bit 4:4:4 (identity GBR) — NVENC")
                    val: StreamingPreferences.PLANK_PROFILE_NVENC_HEVC_10BIT_444
                }
            }

            Label {
                text: qsTr("Startup encoder target: %1 Mbps").arg(
                          (editBitrateSlider.value / 1000.0).toFixed(1))
                font.bold: true
            }

            Slider {
                id: editBitrateSlider
                Layout.fillWidth: true
                from: StreamingPreferences.plankBitrateMinimumKbps()
                to: StreamingPreferences.plankBitrateMaximumKbps()
                stepSize: StreamingPreferences.plankBitrateStepKbps()
                snapMode: Slider.SnapAlways
                onMoved: editBookmarkDialog.rememberProfileBitrate()
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Saved independently for each encoding profile. Toolbar adjustments apply only to the active session.")
                wrapMode: Text.Wrap
                opacity: 0.72
            }

            Label {
                text: qsTr("Host display layout")
                font.bold: true
            }
            PlankComboBox {
                id: editHostLayout
                Layout.fillWidth: true
                model: editCaptureSource.captureSource === 2 ? [qsTr("Match client display(s)"), qsTr("One Mac virtual display")] : [
                    qsTr("Match client displays"),
                    qsTr("Physical displays"),
                    qsTr("One virtual display"),
                    qsTr("Two virtual displays (horizontal)")
                ]
            }

            Label {
                Layout.fillWidth: true
                visible: editCaptureSource.captureSource !== 2 && editBookmarkDialog.hostDisplayPolicy === 0
                text: qsTr("This headless workstation does not provide physical displays.")
                wrapMode: Text.Wrap
                opacity: 0.72
            }

            Label {
                text: editCaptureSource.captureSource === 2 ? qsTr("Mac desktop resolution") : qsTr("Virtual display 1 resolution")
                font.bold: true
                opacity: (editCaptureSource.captureSource === 2 ? editHostLayout.currentIndex === 1 : editHostLayout.currentIndex >= 2) ? 1.0 : 0.5
            }
            PlankComboBox {
                id: editVirtualMode1
                Layout.fillWidth: true
                enabled: (editCaptureSource.captureSource === 2 ? editHostLayout.currentIndex === 1 : editHostLayout.currentIndex >= 2)
                model: editBookmarkDialog.virtualModeChoices
                delegate: ItemDelegate {
                    width: editVirtualMode1.width
                    text: modelData
                    enabled: editBookmarkDialog.virtualModeSupported(modelData)
                    highlighted: editVirtualMode1.highlightedIndex === index
                }
            }

            Label {
                visible: editCaptureSource.captureSource !== 2
                text: qsTr("Virtual display 2 resolution")
                font.bold: true
                opacity: editHostLayout.currentIndex === 3 ? 1.0 : 0.5
            }
            PlankComboBox {
                id: editVirtualMode2
                visible: editCaptureSource.captureSource !== 2
                Layout.fillWidth: true
                enabled: editHostLayout.currentIndex === 3
                model: editBookmarkDialog.virtualModeChoices
                delegate: ItemDelegate {
                    width: editVirtualMode2.width
                    text: modelData
                    enabled: editBookmarkDialog.virtualModeSupported(modelData)
                    highlighted: editVirtualMode2.highlightedIndex === index
                }
            }

            Label {
                text: qsTr("Scaling")
                font.bold: true
            }
            PlankComboBox {
                id: editScalingChoice
                Layout.fillWidth: true
                model: [qsTr("Native (1:1 pixels)"), qsTr("Scaled-Span")]
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Native preserves one host pixel per streamed pixel. Scaled-Span fits the complete host desktop into the client resolution.")
                wrapMode: Text.Wrap
                opacity: 0.72
            }
        }
    }

    NavigableMessageDialog {
        id: deletePcDialog
        // don't allow edits to the rest of the window while open
        property int pcIndex : -1
        property string pcName : ""
        text: qsTr("Are you sure you want to remove '%1'?").arg(pcName)
        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: {
            computerModel.deleteComputer(pcIndex)
        }
    }

    NavigableDialog {
        id: renamePcDialog
        property string label: qsTr("Enter the new name for this PC:")
        property string originalName
        property int pcIndex : -1;

        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            // Force keyboard focus on the textbox so keyboard navigation works
            editText.forceActiveFocus()
        }

        onClosed: {
            editText.clear()
        }

        onAccepted: {
            if (editText.text) {
                computerModel.renameComputer(pcIndex, editText.text)
            }
        }

        ColumnLayout {
            Label {
                text: renamePcDialog.label
                font.bold: true
            }

            PlankTextField {
                id: editText
                placeholderText: renamePcDialog.originalName
                Layout.fillWidth: true
                focus: true

                Keys.onReturnPressed: {
                    renamePcDialog.accept()
                }

                Keys.onEnterPressed: {
                    renamePcDialog.accept()
                }
            }
        }
    }

    ScrollBar.vertical: ScrollBar {}
}
