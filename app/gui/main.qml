import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2
import QtQuick.Controls.Material 2.2

import ComputerManager 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0

ApplicationWindow {
    property bool pollingActive: false

    id: window
    title: qsTr("PLANK Client")
    width: 1280
    height: 1200
    minimumHeight: 900

    PlankTheme {
        id: theme
    }

    Material.theme: Material.Dark
    Material.background: theme.canvas
    Material.foreground: theme.textPrimary
    Material.primary: theme.chrome
    Material.accent: theme.accent

    Component.onCompleted: {
        // The PLANK launcher is always a normal desktop window.
        window.show()

        // Display any modal dialogs for configuration warnings
        if (SystemProperties.isWow64) {
            wow64Dialog.open()
        }
        else if (!SystemProperties.hasHardwareAcceleration) {
            if (SystemProperties.isRunningXWayland) {
                xWaylandDialog.open()
            }
            else {
                noHwDecoderDialog.open()
            }
        }

    }
  
    // ToolTip is an attached property and must be hosted on an Item rather
    // than directly on ApplicationWindow under Qt 6.10.
    Item {
        visible: false
        width: 0
        height: 0

        Text {
            id: tooltipTextLayoutHelper
            visible: false
            font: ToolTip.toolTip.font
            text: ToolTip.toolTip.text
        }

        ToolTip.toolTip.contentWidth: Math.min(tooltipTextLayoutHelper.width, 400)
    }

    function goBack() {
        stackView.pop()
    }

    StackView {
        id: stackView
        initialItem: initialView
        anchors.fill: parent
        focus: true

        onCurrentItemChanged: {
            // Ensure focus travels to the next view when going back
            if (currentItem) {
                currentItem.forceActiveFocus()
            }
        }

        Keys.onEscapePressed: {
            if (depth > 1) {
                goBack()
            }
            else {
                quitConfirmationDialog.open()
            }
        }

        Keys.onBackPressed: {
            if (depth > 1) {
                goBack()
            }
            else {
                quitConfirmationDialog.open()
            }
        }

        Keys.onMenuPressed: {
            settingsButton.clicked()
        }

    }

    // This timer keeps us polling for 5 minutes of inactivity
    // to allow the user to work with Moonlight on a second display
    // while dealing with configuration issues. This will ensure
    // machines come online even if the input focus isn't on Moonlight.
    Timer {
        id: inactivityTimer
        interval: 5 * 60000
        onTriggered: {
            if (!active && pollingActive) {
                ComputerManager.stopPollingAsync()
                pollingActive = false
            }
        }
    }

    onVisibleChanged: {
        // When we become invisible while streaming is going on,
        // stop polling immediately.
        if (!visible) {
            inactivityTimer.stop()

            if (pollingActive) {
                ComputerManager.stopPollingAsync()
                pollingActive = false
            }
        }
        else if (active) {
            // When we become visible and active again, start polling
            inactivityTimer.stop()

            // Restart polling if it was stopped
            if (!pollingActive) {
                ComputerManager.startPolling()
                pollingActive = true
            }
        }
    }

    onActiveChanged: {
        if (active) {
            // Stop the inactivity timer
            inactivityTimer.stop()

            // Restart polling if it was stopped
            if (!pollingActive) {
                ComputerManager.startPolling()
                pollingActive = true
            }
        }
        else {
            // Start the inactivity timer to stop polling
            // if focus does not return within a few minutes.
            inactivityTimer.restart()
        }
    }

    // Workaround for lack of instanceof in Qt 5.9.
    //
    // Based on https://stackoverflow.com/questions/13923794/how-to-do-a-is-a-typeof-or-instanceof-in-qml
    function qmltypeof(obj, className) { // QtObject, string -> bool
        if (obj === null || obj === undefined) {
            return false
        }
        // className plus "(" is the class instance without modification
        // className plus "_QML" is the class instance with user-defined properties
        var str = obj.toString();
        return str.startsWith(className + "(") || str.startsWith(className + "_QML");
    }

    function navigateTo(url, objectType)
    {
        var existingItem = stackView.find(function(item, index) {
            return qmltypeof(item, objectType)
        })

        if (existingItem !== null) {
            // Pop to the existing item
            stackView.pop(existingItem)
        }
        else {
            // Create a new item
            stackView.push(url)
        }
    }

    header: ToolBar {
        id: toolBar
        height: 56

        background: Rectangle {
            color: theme.chrome

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: theme.borderSubtle
            }
        }

        Label {
            id: titleLabel
            visible: toolBar.width > 700
            anchors.fill: parent
            text: stackView.currentItem ? stackView.currentItem.objectName : ""
            color: theme.textPrimary
            font.pointSize: 15
            font.weight: Font.DemiBold
            elide: Label.ElideRight
            horizontalAlignment: Qt.AlignHCenter
            verticalAlignment: Qt.AlignVCenter
        }

        RowLayout {
            spacing: theme.spaceSmall
            anchors.leftMargin: theme.spaceMedium
            anchors.rightMargin: theme.spaceMedium
            anchors.fill: parent

            NavigableToolButton {
                // Only make the button visible if the user has navigated somewhere.
                visible: stackView.depth > 1

                iconSource: "qrc:/res/arrow_left.svg"

                onClicked: goBack()

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            PlankVersionButton {
                version: SystemProperties.plankVersionString
                changelog: SystemProperties.plankChangelog
            }

            // This label will appear when the window gets too small and
            // we need to ensure the toolbar controls don't collide
            Label {
                id: titleRowLabel
                font.pointSize: titleLabel.font.pointSize
                elide: Label.ElideRight
                horizontalAlignment: Qt.AlignHCenter
                verticalAlignment: Qt.AlignVCenter
                Layout.fillWidth: true

                // We need this label to always be visible so it can occupy
                // the remaining space in the RowLayout. To "hide" it, we
                // just set the text to empty string.
                text: !titleLabel.visible && stackView.currentItem ? stackView.currentItem.objectName : ""
            }

            NavigableToolButton {
                id: facilitySignInButton
                // Uppercut: bookmarks mode only; with a broker the sign-in
                // screen is already underneath the workstation list.
                visible: qmltypeof(stackView.currentItem, "PcView") && stackView.depth === 1

                iconSource: "qrc:/res/baseline-lock-24px.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Sign in to a facility")

                onClicked: stackView.push("qrc:/gui/FacilitySignInView.qml")

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            NavigableToolButton {
                id: addPcButton
                visible: qmltypeof(stackView.currentItem, "PcView") && stackView.depth === 1

                iconSource:  "qrc:/res/ic_add_to_queue_white_48px.svg"

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Add PC manually") + (newPcShortcut.nativeText ? (" ("+newPcShortcut.nativeText+")") : "")

                Shortcut {
                    id: newPcShortcut
                    sequence: StandardKey.New
                    onActivated: addPcButton.clicked()
                }

                onClicked: {
                    addPcDialog.open()
                }

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }
            }

            NavigableToolButton {
                id: settingsButton

                iconSource:  "qrc:/res/settings.svg"

                onClicked: navigateTo("qrc:/gui/SettingsView.qml", "SettingsView")

                Keys.onDownPressed: {
                    stackView.currentItem.forceActiveFocus(Qt.TabFocus)
                }

                Shortcut {
                    id: settingsShortcut
                    sequence: StandardKey.Preferences
                    onActivated: settingsButton.clicked()
                }

                ToolTip.delay: 1000
                ToolTip.timeout: 3000
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Settings") + (settingsShortcut.nativeText ? (" ("+settingsShortcut.nativeText+")") : "")
            }
        }
    }

    ErrorMessageDialog {
        id: noHwDecoderDialog
        text: qsTr("No functioning hardware accelerated video decoder was detected by PLANK. " +
                   "Your streaming performance may be severely degraded in this configuration.")
    }

    ErrorMessageDialog {
        id: xWaylandDialog
        text: qsTr("Hardware acceleration doesn't work on XWayland. Continuing on XWayland may result in poor streaming performance. " +
                   "Try running with QT_QPA_PLATFORM=wayland or switch to X11.")
    }

    NavigableMessageDialog {
        id: wow64Dialog
        standardButtons: Dialog.Ok
        text: qsTr("This PLANK build isn't optimized for your PC. Please install the '%1' PLANK package for the best streaming performance.").arg(SystemProperties.friendlyNativeArchName)
    }

    // This dialog appears when quitting via keyboard.
    NavigableMessageDialog {
        id: quitConfirmationDialog
        standardButtons: Dialog.Yes | Dialog.No
        text: qsTr("Are you sure you want to quit?")
        // For keyboard navigation
        onAccepted: Qt.quit()
    }

    // HACK: This belongs in StreamSegue but keeping a dialog around after the parent
    // dies can trigger bugs in Qt 5.12 that cause the app to crash. For now, we will
    // host this dialog in a QML component that is never destroyed.
    //
    // To repro: Start a stream, cut the network connection to trigger the "Connection
    // terminated" dialog, wait until the app grid times out back to the PC grid, then
    // try to dismiss the dialog.
    ErrorMessageDialog {
        id: streamSegueErrorDialog

        property bool quitAfter: false

        onClosed: {
            if (quitAfter) {
                Qt.quit()
            }

            // StreamSegue assumes its dialog will be re-created each time we
            // start streaming, so fake it by wiping out the text each time.
            text = ""
        }
    }

    NavigableDialog {
        id: addPcDialog
        title: qsTr("Add workstation bookmark")
        property var virtualModeChoices: ComputerManager.plankVirtualModeChoices()
        property var profileBitratesKbps: []

        // Give both connection fields enough room for real hostnames while
        // keeping the dialog inside smaller launcher windows. The dialog still
        // blocks the launcher, but it must not wash out the UI behind it.
        width: Math.min(640, parent.width - 40)
        height: Math.min(implicitHeight, parent.height - 20)
        dim: false

        function suggestedNickname(address) {
            var value = address.trim()
            if (value === "" || value.charAt(0) === "[" || /^\d{1,3}(\.\d{1,3}){3}(:\d+)?$/.test(value)) {
                return ""
            }
            return value.split(":")[0].split(".")[0]
        }

        function currentVideoProfile() {
            if (addEncodingProfile.currentIndex >= 0 &&
                    addEncodingProfile.currentIndex < addEncodingProfile.model.count) {
                return addEncodingProfile.model.get(
                            addEncodingProfile.currentIndex).val
            }
            return StreamingPreferences.PLANK_PROFILE_NVENC_HEVC_10BIT_444
        }

        function applyProfileBitrate() {
            var profile = currentVideoProfile()
            var saved = profileBitratesKbps[profile]
            addBitrateSlider.value = saved === undefined ?
                        StreamingPreferences.plankDefaultBitrateKbps(profile) : saved
        }

        function rememberProfileBitrate() {
            var values = []
            for (var i = 0; i < profileBitratesKbps.length; ++i) {
                values.push(profileBitratesKbps[i])
            }
            values[currentVideoProfile()] = Math.round(addBitrateSlider.value)
            profileBitratesKbps = values
        }

        function virtualModeSupported(mode) {
            return StreamingPreferences.plankVirtualModeSupportedForProfile(
                        mode, currentVideoProfile())
        }

        function ensureVirtualModesCompatible() {
            if (addCaptureSource.captureSource === 2) return
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
            if (!virtualModeSupported(virtualModeChoices[addVirtualMode1.currentIndex])) {
                addVirtualMode1.currentIndex = fallback
            }
            if (!virtualModeSupported(virtualModeChoices[addVirtualMode2.currentIndex])) {
                addVirtualMode2.currentIndex = fallback
            }
        }

        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            // Force keyboard focus on the textbox so keyboard navigation works
            addressText.forceActiveFocus()
            profileBitratesKbps =
                    StreamingPreferences.plankDefaultProfileBitratesKbps()
            applyProfileBitrate()
            standardButton(Dialog.Ok).enabled = Qt.binding(function() {
                return addressText.text.trim() !== "" && nicknameText.text.trim() !== ""
            })
        }

        onClosed: {
            addressText.clear()
            nicknameText.clear()
            nicknameText.manuallyEdited = false
            addHostLayout.currentIndex = 0
            addVirtualMode1.currentIndex = 9
            addVirtualMode2.currentIndex = 1
            addScalingChoice.currentIndex = 1
            addCaptureSource.selectCaptureSource(0)
            addEncodingProfile.currentIndex = 6
            profileBitratesKbps = []
        }

        onAccepted: {
            if (addressText.text && nicknameText.text) {
                ComputerManager.addNewHostManually(addressText.text.trim(),
                                                   nicknameText.text.trim(),
                                                   addHostLayout.currentIndex,
                                                   addVirtualMode1.currentIndex,
                                                   addVirtualMode2.currentIndex,
                                                   addScalingChoice.currentIndex,
                                                   addEncodingProfile.model.get(
                                                       addEncodingProfile.currentIndex).val,
                                                   addCaptureSource.captureSource,
                                                   profileBitratesKbps)
            }
        }

        ColumnLayout {
            width: parent.width

            Label {
                text: qsTr("Address or hostname")
                font.bold: true
            }

            PlankTextField {
                id: addressText
                Layout.fillWidth: true
                focus: true

                onTextEdited: {
                    if (!nicknameText.manuallyEdited) {
                        nicknameText.text = addPcDialog.suggestedNickname(text)
                    }
                }

                Keys.onReturnPressed: nicknameText.forceActiveFocus()
                Keys.onEnterPressed: nicknameText.forceActiveFocus()
            }

            Label {
                text: qsTr("Nickname")
                font.bold: true
            }

            PlankTextField {
                id: nicknameText
                property bool manuallyEdited: false

                Layout.fillWidth: true

                onTextEdited: manuallyEdited = true

                Keys.onReturnPressed: {
                    if (addressText.text && text) {
                        addPcDialog.accept()
                    }
                }

                Keys.onEnterPressed: {
                    if (addressText.text && text) {
                        addPcDialog.accept()
                    }
                }
            }

            Label {
                text: qsTr("Capture source")
                font.bold: true
            }

            PlankCaptureSourceBox {
                id: addCaptureSource
                Layout.fillWidth: true
                hostAddress: addressText.text
                probingEnabled: addPcDialog.visible
                onCaptureSourceChanged: {
                    addHostLayout.currentIndex = 0
                    addEncodingProfile.currentIndex = captureSource === 0 ? 6 : 0
                    Qt.callLater(addPcDialog.applyProfileBitrate)
                }
            }

            Label {
                text: qsTr("Encoding profile")
                font.bold: true
            }

            PlankComboBox {
                id: addEncodingProfile
                Layout.fillWidth: true
                textRole: "text"
                currentIndex: 6
                model: addCaptureSource.captureSource === 2 ? addAppleEncodingProfileModel :
                       addCaptureSource.captureSource === 0 ?
                           addNvfbcEncodingProfileModel : addNativeEncodingProfileModel
                onActivated: {
                    addPcDialog.applyProfileBitrate()
                    addPcDialog.ensureVirtualModesCompatible()
                }
            }

            ListModel {
                id: addNvfbcEncodingProfileModel
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
                id: addAppleEncodingProfileModel
                ListElement {
                    text: qsTr("HEVC 10-bit 4:2:0 — Apple VideoToolbox")
                    val: StreamingPreferences.PLANK_PROFILE_APPLE_HEVC_10BIT_420
                }
                ListElement {
                    text: qsTr("HEVC 10-bit 4:4:4 — Apple VideoToolbox")
                    val: StreamingPreferences.PLANK_PROFILE_APPLE_HEVC_10BIT_444
                }
            }

            ListModel {
                id: addNativeEncodingProfileModel
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
                          (addBitrateSlider.value / 1000.0).toFixed(1))
                font.bold: true
            }

            Slider {
                id: addBitrateSlider
                Layout.fillWidth: true
                from: StreamingPreferences.plankBitrateMinimumKbps()
                to: StreamingPreferences.plankBitrateMaximumKbps()
                stepSize: StreamingPreferences.plankBitrateStepKbps()
                snapMode: Slider.SnapAlways
                onMoved: addPcDialog.rememberProfileBitrate()
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
                id: addHostLayout
                Layout.fillWidth: true
                model: addCaptureSource.captureSource === 2 ? [qsTr("Match client display(s)"), qsTr("One Mac virtual display")] : [
                    qsTr("Match client displays"),
                    qsTr("Match Host"),
                    qsTr("One virtual display"),
                    qsTr("Two virtual displays (horizontal)")
                ]
            }

            Label {
                text: addCaptureSource.captureSource === 2 ? qsTr("Mac desktop resolution") : qsTr("Virtual display 1 resolution")
                font.bold: true
                opacity: (addCaptureSource.captureSource === 2 ? addHostLayout.currentIndex === 1 : addHostLayout.currentIndex >= 2) ? 1.0 : 0.5
            }

            PlankComboBox {
                id: addVirtualMode1
                Layout.fillWidth: true
                enabled: (addCaptureSource.captureSource === 2 ? addHostLayout.currentIndex === 1 : addHostLayout.currentIndex >= 2)
                currentIndex: 9
                model: addPcDialog.virtualModeChoices
                delegate: ItemDelegate {
                    width: addVirtualMode1.width
                    text: modelData
                    enabled: addPcDialog.virtualModeSupported(modelData)
                    highlighted: addVirtualMode1.highlightedIndex === index
                }
            }

            Label {
                visible: addCaptureSource.captureSource !== 2
                text: qsTr("Virtual display 2 resolution")
                font.bold: true
                opacity: addHostLayout.currentIndex === 3 ? 1.0 : 0.5
            }

            PlankComboBox {
                id: addVirtualMode2
                visible: addCaptureSource.captureSource !== 2
                Layout.fillWidth: true
                enabled: addHostLayout.currentIndex === 3
                currentIndex: 1
                model: addPcDialog.virtualModeChoices
                delegate: ItemDelegate {
                    width: addVirtualMode2.width
                    text: modelData
                    enabled: addPcDialog.virtualModeSupported(modelData)
                    highlighted: addVirtualMode2.highlightedIndex === index
                }
            }

            Label {
                text: qsTr("Scaling")
                font.bold: true
            }

            PlankComboBox {
                id: addScalingChoice
                Layout.fillWidth: true
                currentIndex: 1
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
}
