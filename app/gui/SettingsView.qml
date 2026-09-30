import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2
import QtQuick.Window 2.2

import StreamingPreferences 1.0
import ComputerManager 1.0
import SystemProperties 1.0

Flickable {
    id: settingsPage
    objectName: qsTr("Settings")

    PlankTheme {
        id: theme
    }

    signal languageChanged()

    boundsBehavior: Flickable.OvershootBounds

    contentWidth: width
    contentHeight: settingsColumn1.height > settingsColumn2.height ? settingsColumn1.height : settingsColumn2.height

    ScrollBar.vertical: ScrollBar {
        anchors {
            left: parent.right
            leftMargin: -10
        }
    }

    function isChildOfFlickable(item) {
        while (item) {
            if (item.parent === contentItem) {
                return true
            }

            item = item.parent
        }
        return false
    }

    NumberAnimation on contentY {
        id: autoScrollAnimation
        duration: 100
    }

    Window.onActiveFocusItemChanged: {
        var item = Window.activeFocusItem
        if (item) {
            // Ignore non-child elements like the toolbar buttons
            if (!isChildOfFlickable(item)) {
                return
            }

            // Map the focus item's position into our content item's coordinate space
            var pos = item.mapToItem(contentItem, 0, 0)

            // Ensure some extra space is visible around the element we're scrolling to
            var scrollMargin = height > 100 ? 50 : 0

            if (pos.y - scrollMargin < contentY) {
                autoScrollAnimation.from = contentY
                autoScrollAnimation.to = Math.max(pos.y - scrollMargin, 0)
                autoScrollAnimation.start()
            }
            else if (pos.y + item.height + scrollMargin > contentY + height) {
                autoScrollAnimation.from = contentY
                autoScrollAnimation.to = Math.min(pos.y + item.height + scrollMargin - height, contentHeight - height)
                autoScrollAnimation.start()
            }
        }
    }

    StackView.onDeactivating: {
        // Save the prefs so the Session can observe the changes
        StreamingPreferences.save()
    }

    Component.onDestruction: {
        // Also save preferences on destruction, since we won't get a
        // deactivating callback if the user just closes Moonlight
        StreamingPreferences.save()
    }

    Column {
        padding: 20
        rightPadding: 10
        id: settingsColumn1
        width: settingsPage.width / 2
        spacing: theme.spaceMedium

        PlankSection {
            id: basicSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            title: qsTr("Basic Settings")

            PlankSettingsGrid {

                PlankSettingLabel {
                    id: frameRateTitle
                    text: qsTr("Frame rate")
                }

                AutoResizingComboBox {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        property int lastIndexValue

                        function updateFrameRateForSelection() {
                            var selectedFps = parseInt(model.get(fpsComboBox.currentIndex).video_fps)
                            if (StreamingPreferences.fps !== selectedFps) {
                                StreamingPreferences.fps = selectedFps
                            }

                            lastIndexValue = currentIndex
                        }

                        NavigableDialog {
                            function isInputValid() {
                                // If we have text that isn't valid, reject the input.
                                if (!fpsField.acceptableInput && fpsField.text) {
                                    return false
                                }

                                // The textbox needs to have text or placeholder text
                                if (!fpsField.text && !fpsField.placeholderText) {
                                    return false
                                }

                                return true
                            }

                            id: customFpsDialog
                            standardButtons: Dialog.Ok | Dialog.Cancel
                            onOpened: {
                                // Force keyboard focus on the textbox so keyboard navigation works
                                fpsField.forceActiveFocus()

                                // standardButton() was added in Qt 5.10, so we must check for it first
                                if (customFpsDialog.standardButton) {
                                    customFpsDialog.standardButton(Dialog.Ok).enabled = customFpsDialog.isInputValid()
                                }
                            }

                            onClosed: {
                                fpsField.clear()
                            }

                            onRejected: {
                                fpsComboBox.currentIndex = fpsComboBox.lastIndexValue
                            }

                            onAccepted: {
                                // Reject if there's invalid input
                                if (!isInputValid()) {
                                    reject()
                                    return
                                }

                                var fps = fpsField.text ? fpsField.text : fpsField.placeholderText

                                // Find and update the custom entry
                                for (var i = 0; i < fpsListModel.count; i++) {
                                    if (fpsListModel.get(i).is_custom) {
                                        fpsListModel.setProperty(i, "video_fps", fps)
                                        fpsListModel.setProperty(i, "text", qsTr("Custom (%1 FPS)").arg(fps))

                                        // Apply the custom frame rate.
                                        fpsComboBox.currentIndex = i
                                        fpsComboBox.updateFrameRateForSelection()

                                        // Update the combobox width too
                                        fpsComboBox.recalculateWidth()
                                        break
                                    }
                                }
                            }

                            ColumnLayout {
                                Label {
                                    text: qsTr("Enter a custom frame rate:")
                                    font.bold: true
                                }

                                RowLayout {
                                    PlankTextField {
                                        id: fpsField
                                        maximumLength: 4
                                        inputMethodHints: Qt.ImhDigitsOnly
                                        placeholderText: fpsListModel.get(fpsComboBox.currentIndex).video_fps
                                        validator: IntValidator{bottom:10; top:9999}
                                        focus: true

                                        onTextChanged: {
                                            // standardButton() was added in Qt 5.10, so we must check for it first
                                            if (customFpsDialog.standardButton) {
                                                customFpsDialog.standardButton(Dialog.Ok).enabled = customFpsDialog.isInputValid()
                                            }
                                        }

                                        Keys.onReturnPressed: {
                                            customFpsDialog.accept()
                                        }

                                        Keys.onEnterPressed: {
                                            customFpsDialog.accept()
                                        }
                                    }
                                }
                            }
                        }

                        function addRefreshRateOrdered(fpsListModel, refreshRate, description, custom) {
                            var indexToAdd = 0
                            for (var j = 0; j < fpsListModel.count; j++) {
                                var existing_fps = parseInt(fpsListModel.get(j).video_fps);

                                if (refreshRate === existing_fps || (custom && fpsListModel.get(j).is_custom)) {
                                    // Duplicate entry, skip
                                    indexToAdd = -1
                                    break
                                }
                                else if (refreshRate > existing_fps) {
                                    // Candidate entrypoint after this entry
                                    indexToAdd = j + 1
                                }
                            }

                            // Insert this frame rate if it's not a duplicate
                            if (indexToAdd >= 0) {
                                // Custom values always go at the end of the list
                                if (custom) {
                                    indexToAdd = fpsListModel.count
                                }

                                fpsListModel.insert(indexToAdd,
                                                    {
                                                        "text": description,
                                                        "video_fps": ""+refreshRate,
                                                        "is_custom": custom
                                                    })
                            }

                            return indexToAdd
                        }

                        function reinitialize() {
                            // Add native refresh rate for all attached displays
                            var done = false
                            for (var displayIndex = 0; !done; displayIndex++) {
                                var refreshRate = SystemProperties.getRefreshRate(displayIndex);
                                if (refreshRate === 0) {
                                    // Exceeded max count of displays
                                    done = true
                                    break
                                }

                                addRefreshRateOrdered(fpsListModel, refreshRate, qsTr("%1 FPS").arg(refreshRate), false)
                            }

                            var saved_fps = StreamingPreferences.fps
                            var found = false
                            for (var i = 0; i < model.count; i++) {
                                var el_fps = parseInt(model.get(i).video_fps);

                                // Look for a matching frame rate
                                if (saved_fps === el_fps) {
                                    currentIndex = i
                                    found = true
                                    break
                                }
                            }

                            // If we didn't find one, add a custom frame rate for the current value
                            if (!found) {
                                currentIndex = addRefreshRateOrdered(model, saved_fps, qsTr("Custom (%1 FPS)").arg(saved_fps), true)
                            }
                            else {
                                addRefreshRateOrdered(model, "", qsTr("Custom"), true)
                            }

                            recalculateWidth()

                            lastIndexValue = currentIndex
                        }

                        // ignore setting the index at first, and actually set it when the component is loaded
                        Component.onCompleted: {
                            reinitialize()
                            languageChanged.connect(reinitialize)
                        }

                        model: ListModel {
                            id: fpsListModel
                            // Other elements may be added at runtime
                            ListElement {
                                text: qsTr("30 FPS")
                                video_fps: "30"
                                is_custom: false
                            }
                            ListElement {
                                text: qsTr("60 FPS")
                                video_fps: "60"
                                is_custom: false
                            }
                        }

                        id: fpsComboBox
                        textRole: "text"
                        // ::onActivated must be used, as it only listens for when the index is changed by a human
                        onActivated : {
                            if (model.get(currentIndex).is_custom) {
                                customFpsDialog.open()
                            }
                            else {
                                updateFrameRateForSelection()
                            }
                        }
                }

                PlankSettingLabel {
                    id: windowModeTitle
                    text: qsTr("Window Mode")
                    visible: SystemProperties.hasDesktopEnvironment
                }

                AutoResizingComboBox {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    function createModel() {
                        var model = Qt.createQmlObject('import QtQuick 2.0; ListModel {}', parent, '')

                        model.append({
                                         text: qsTr("Borderless windowed"),
                                         val: StreamingPreferences.WM_FULLSCREEN_DESKTOP
                                     })

                        model.append({
                                         text: qsTr("Windowed"),
                                         val: StreamingPreferences.WM_WINDOWED
                                     })


                        // Set the recommended option based on the OS
                        for (var i = 0; i < model.count; i++) {
                            var thisWm = model.get(i).val;
                            if (thisWm === StreamingPreferences.recommendedFullScreenMode) {
                                model.get(i).text += " " + qsTr("(Recommended)")
                                model.move(i, 0, 1)
                                break
                            }
                        }

                        return model
                    }


                    // This is used on initialization and upon retranslation
                    function reinitialize() {
                        if (!visible) {
                            // Do nothing if the control won't even be visible
                            return
                        }

                        model = createModel()
                        currentIndex = 0

                        // Set the current value based on the saved preferences
                        var savedWm = StreamingPreferences.windowMode
                        for (var i = 0; i < model.count; i++) {
                             var thisWm = model.get(i).val;
                             if (savedWm === thisWm) {
                                 currentIndex = i
                                 break
                             }
                        }

                        activated(currentIndex)
                    }

                    Component.onCompleted: {
                        reinitialize()
                        languageChanged.connect(reinitialize)
                    }

                    id: windowModeComboBox
                    visible: SystemProperties.hasDesktopEnvironment
                    enabled: !SystemProperties.rendererAlwaysFullScreen
                    hoverEnabled: true
                    textRole: "text"
                    onActivated: {
                        StreamingPreferences.windowMode = model.get(currentIndex).val
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Borderless fills the display. Windowed provides a movable, resizable stream window with desktop decorations.")
                }

                PlankSettingLabel {
                    text: qsTr("Broadcast monitor")
                    visible: SystemProperties.hasDesktopEnvironment
                }

                AutoResizingComboBox {
                    id: broadcastMonitorComboBox
                    Layout.fillWidth: true
                    visible: SystemProperties.hasDesktopEnvironment
                    textRole: "text"
                    function reinitialize() {
                        if (!visible) return
                        var choices = StreamingPreferences.localMonitors()
                        var choiceModel = Qt.createQmlObject('import QtQuick 2.0; ListModel {}', parent, '')
                        var saved = StreamingPreferences.broadcastMonitorGeometry
                        var selected = 0
                        for (var i = 0; i < choices.length; i++) {
                            choiceModel.append({
                                             text: choices[i].text,
                                             geometry: choices[i].geometry
                                         })
                            if (choices[i].geometry === saved) selected = i
                        }
                        model = choiceModel
                        currentIndex = selected
                    }
                    Component.onCompleted: reinitialize()
                    onActivated: {
                        StreamingPreferences.broadcastMonitorGeometry = model.get(currentIndex).geometry
                    }
                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("This monitor shows the Flame NDI picture through UltraGrid. The other monitors stay the remote desktop.")
                }

                PlankSettingLabel {
                    text: qsTr("V-Sync")
                }

                PlankCheckBox {
                    id: vsyncCheck
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                    hoverEnabled: true
                    text: ""
                    Accessible.name: qsTr("V-Sync")
                    checked: StreamingPreferences.enableVsync
                    onCheckedChanged: {
                        StreamingPreferences.enableVsync = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Disabling V-Sync allows sub-frame rendering latency, but it can display visible tearing")
                }

            }
        }

        PlankSection {

            id: audioSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            title: qsTr("Audio Settings")

            PlankSettingsGrid {

                PlankSettingLabel {
                    id: resAudioTitle
                    text: qsTr("Audio configuration")
                }

                AutoResizingComboBox {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_audio = StreamingPreferences.audioConfig
                        currentIndex = 0
                        for (var i = 0; i < audioListModel.count; i++) {
                            var el_audio = audioListModel.get(i).val;
                            if (saved_audio === el_audio) {
                                currentIndex = i
                                break
                            }
                        }
                        activated(currentIndex)
                    }

                    id: audioComboBox
                    textRole: "text"
                    model: ListModel {
                        id: audioListModel
                        ListElement {
                            text: qsTr("Stereo")
                            val: StreamingPreferences.AC_STEREO
                        }
                        ListElement {
                            text: qsTr("5.1 surround sound")
                            val: StreamingPreferences.AC_51_SURROUND
                        }
                        ListElement {
                            text: qsTr("7.1 surround sound")
                            val: StreamingPreferences.AC_71_SURROUND
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        StreamingPreferences.audioConfig = audioListModel.get(currentIndex).val
                    }
                }

                PlankSettingLabel {
                    text: qsTr("Mute host PC speakers while streaming")
                }

                PlankCheckBox {
                    id: audioPcCheck
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                    text: ""
                    Accessible.name: qsTr("Mute host PC speakers while streaming")
                    checked: !StreamingPreferences.playAudioOnHost
                    onCheckedChanged: {
                        StreamingPreferences.playAudioOnHost = !checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("You must restart any game currently in progress for this setting to take effect")
                }

                PlankSettingLabel {
                    text: qsTr("Mute audio when the client is inactive")
                    visible: SystemProperties.hasDesktopEnvironment
                }

                PlankCheckBox {
                    id: muteOnFocusLossCheck
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                    text: ""
                    Accessible.name: qsTr("Mute audio stream when the client is not the active window")
                    visible: SystemProperties.hasDesktopEnvironment
                    checked: StreamingPreferences.muteOnFocusLoss
                    onCheckedChanged: {
                        StreamingPreferences.muteOnFocusLoss = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Mutes streamed audio when you Alt+Tab out of the stream or click on a different window.")
                }
            }
        }

        PlankSection {
            id: uiSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            title: qsTr("UI Settings")

            PlankSettingsGrid {

                PlankSettingLabel {
                    id: languageTitle
                    text: qsTr("Language")
                }

                AutoResizingComboBox {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_language = StreamingPreferences.language
                        currentIndex = 0
                        for (var i = 0; i < languageListModel.count; i++) {
                            var el_language = languageListModel.get(i).val;
                            if (saved_language === el_language) {
                                currentIndex = i
                                break
                            }
                        }

                        activated(currentIndex)
                    }

                    id: languageComboBox
                    textRole: "text"
                    model: ListModel {
                        id: languageListModel
                        ListElement {
                            text: qsTr("Automatic")
                            val: StreamingPreferences.LANG_AUTO
                        }
                        ListElement {
                            text: "Deutsch" // German
                            val: StreamingPreferences.LANG_DE
                        }
                        ListElement {
                            text: "English"
                            val: StreamingPreferences.LANG_EN
                        }
                        ListElement {
                            text: "Français" // French
                            val: StreamingPreferences.LANG_FR
                        }
                        ListElement {
                            text: "简体中文" // Simplified Chinese
                            val: StreamingPreferences.LANG_ZH_CN
                        }
                        ListElement {
                            text: "Norwegian Bokmål"
                            val: StreamingPreferences.LANG_NB_NO
                        }
                        ListElement {
                            text: "русский" // Russian
                            val: StreamingPreferences.LANG_RU
                        }
                        ListElement {
                            text: "Español" // Spanish
                            val: StreamingPreferences.LANG_ES
                        }
                        ListElement {
                            text: "日本語" // Japanese
                            val: StreamingPreferences.LANG_JA
                        }
                        ListElement {
                            text: "Tiếng Việt" // Vietnamese
                            val: StreamingPreferences.LANG_VI
                        }
                        ListElement {
                            text: "ภาษาไทย" // Thai
                            val: StreamingPreferences.LANG_TH
                        }
                        ListElement {
                            text: "한국어" // Korean
                            val: StreamingPreferences.LANG_KO
                        }
                        ListElement {
                            text: "Magyar" // Hungarian
                            val: StreamingPreferences.LANG_HU
                        }
                        ListElement {
                            text: "Nederlands" // Dutch
                            val: StreamingPreferences.LANG_NL
                        }
                        ListElement {
                            text: "Svenska" // Swedish
                            val: StreamingPreferences.LANG_SV
                        }
                        ListElement {
                            text: "Türkçe" // Turkish
                            val: StreamingPreferences.LANG_TR
                        }
                        /* ListElement {
                            text: "Українська" // Ukrainian
                            val: StreamingPreferences.LANG_UK
                        } */
                        ListElement {
                            text: "繁體中文" // Traditional Chinese
                            val: StreamingPreferences.LANG_ZH_TW
                        }
                        ListElement {
                            text: "Português" // Portuguese
                            val: StreamingPreferences.LANG_PT
                        }
                        /* ListElement {
                            text: "Português do Brasil" // Brazilian Portuguese
                            val: StreamingPreferences.LANG_PT_BR
                        } */
                        ListElement {
                            text: "Ελληνικά" // Greek
                            val: StreamingPreferences.LANG_EL
                        }
                        ListElement {
                            text: "Italiano" // Italian
                            val: StreamingPreferences.LANG_IT
                        }
                        /* ListElement {
                            text: "हिन्दी, हिंदी" // Hindi
                            val: StreamingPreferences.LANG_HI
                        } */
                        ListElement {
                            text: "Język polski" // Polish
                            val: StreamingPreferences.LANG_PL
                        }
                        ListElement {
                            text: "Čeština" // Czech
                            val: StreamingPreferences.LANG_CS
                        }
                        /* ListElement {
                            text: "עִבְרִית" // Hebrew
                            val: StreamingPreferences.LANG_HE
                        } */
                        /* ListElement {
                            text: "کرمانجیی خواروو" // Central Kurdish
                            val: StreamingPreferences.LANG_CKB
                        } */
                        /* ListElement {
                            text: "Lietuvių kalba" // Lithuanian
                            val: StreamingPreferences.LANG_LT
                        } */
                        /* ListElement {
                            text: "Eesti" // Estonian
                            val: StreamingPreferences.LANG_ET
                        } */
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        // Retranslating is expensive, so only do it if the language actually changed
                        var new_language = languageListModel.get(currentIndex).val
                        if (StreamingPreferences.language !== new_language) {
                            StreamingPreferences.language = languageListModel.get(currentIndex).val
                            if (!StreamingPreferences.retranslate()) {
                                ToolTip.show(qsTr("You must restart PLANK Client for this change to take effect"), 5000)
                            }
                            else {
                                // Signal other controls to adjust their text
                                languageChanged()
                            }
                        }
                    }
                }

                PlankSettingLabel {
                    text: qsTr("Connection quality warnings")
                }

                PlankCheckBox {
                    id: connectionWarningsCheck
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                    text: ""
                    Accessible.name: qsTr("Show connection quality warnings")
                    checked: StreamingPreferences.connectionWarnings
                    onCheckedChanged: {
                        StreamingPreferences.connectionWarnings = checked
                    }
                }

                PlankSettingLabel {
                    text: qsTr("Keep display awake while streaming")
                }

                PlankCheckBox {
                    id: keepAwakeCheck
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                    text: ""
                    Accessible.name: qsTr("Keep the display awake while streaming")
                    checked: StreamingPreferences.keepAwake
                    onCheckedChanged: {
                        StreamingPreferences.keepAwake = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Prevents the screensaver from starting or the display from going to sleep while streaming.")
                }
            }
        }
    }

    Column {
        padding: 20
        leftPadding: 10
        anchors.left: settingsColumn1.right
        id: settingsColumn2
        width: settingsPage.width / 2
        spacing: theme.spaceMedium

        PlankSection {
            id: inputSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            title: qsTr("Input Settings")

            PlankSettingsGrid {

                PlankSettingLabel {
                    text: qsTr("Capture system keyboard shortcuts")
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: theme.spaceMedium

                    PlankCheckBox {
                        id: captureSysKeysCheck
                        Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        hoverEnabled: true
                        text: ""
                        Accessible.name: qsTr("Capture system keyboard shortcuts")
                        enabled: SystemProperties.hasDesktopEnvironment
                        checked: StreamingPreferences.captureSysKeysMode !== StreamingPreferences.CSK_OFF || !SystemProperties.hasDesktopEnvironment

                        ToolTip.delay: 1000
                        ToolTip.timeout: 10000
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("This enables the capture of system-wide keyboard shortcuts like Alt+Tab that would normally be handled by the client OS while streaming.") + "\n\n" +
                                      (Qt.platform.os === "osx" ?
                                       qsTr("Requires Accessibility permission for PLANK Client in System Settings > Privacy & Security. Capture applies only to the focused stream. Ctrl+Alt+Shift+Z releases capture; trackpad Spaces gestures stay local.") :
                                       qsTr("NOTE: Certain keyboard shortcuts like Ctrl+Alt+Del on Windows cannot be intercepted by any application, including PLANK Client."))
                    }

                    AutoResizingComboBox {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        // ignore setting the index at first, and actually set it when the component is loaded
                        Component.onCompleted: {
                            if (!visible) {
                                // Do nothing if the control won't even be visible
                                return
                            }

                            var saved_syskeysmode = StreamingPreferences.captureSysKeysMode
                            currentIndex = 0
                            for (var i = 0; i < captureSysKeysModeListModel.count; i++) {
                                var el_syskeysmode = captureSysKeysModeListModel.get(i).val;
                                if (saved_syskeysmode === el_syskeysmode) {
                                    currentIndex = i
                                    break
                                }
                            }

                            activated(currentIndex)
                        }

                        enabled: captureSysKeysCheck.checked && captureSysKeysCheck.enabled
                        textRole: "text"
                        model: ListModel {
                            id: captureSysKeysModeListModel
                            ListElement {
                                text: qsTr("in fullscreen")
                                val: StreamingPreferences.CSK_FULLSCREEN
                            }
                            ListElement {
                                text: qsTr("always")
                                val: StreamingPreferences.CSK_ALWAYS
                            }
                        }

                        function updatePref() {
                            if (!enabled) {
                                StreamingPreferences.captureSysKeysMode = StreamingPreferences.CSK_OFF
                            }
                            else {
                                StreamingPreferences.captureSysKeysMode = captureSysKeysModeListModel.get(currentIndex).val
                            }
                        }

                        // ::onActivated must be used, as it only listens for when the index is changed by a human
                        onActivated: {
                            updatePref()
                        }

                        // This handles transition of the checkbox state
                        onEnabledChanged: {
                            updatePref()
                        }
                    }
                }

            }
        }

        PlankSection {
            id: networkSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            title: qsTr("Network Settings")

            PlankSettingsGrid {

                PlankSettingLabel {
                    text: qsTr("Determine QUIC MTU automatically")
                }

                PlankCheckBox {
                    id: automaticQuicMtuCheckBox
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                    text: ""
                    Accessible.name: qsTr("Determine QUIC MTU automatically")
                    checked: StreamingPreferences.quicUdpPayloadMtu === 0
                    onClicked: {
                        StreamingPreferences.quicUdpPayloadMtu = checked ? 0 : quicMtuSpinBox.value
                    }
                }

                PlankSettingLabel {
                    text: qsTr("Maximum QUIC UDP payload")
                }

                SpinBox {
                    id: quicMtuSpinBox
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    from: 1200
                    to: 65527
                    stepSize: 1
                    editable: true
                    enabled: !automaticQuicMtuCheckBox.checked
                    value: StreamingPreferences.quicUdpPayloadMtu === 0 ? 1344 :
                               StreamingPreferences.quicUdpPayloadMtu
                    onValueModified: {
                        if (enabled) {
                            StreamingPreferences.quicUdpPayloadMtu = value
                        }
                    }
                }

                Item {
                    Layout.preferredWidth: 280
                    Layout.preferredHeight: 1
                }

                PlankSettingHelp {
                    text: automaticQuicMtuCheckBox.checked ?
                              qsTr("Automatic subtracts protocol headers and a 20-byte safety margin from the interface MTU, with a 1344-byte ceiling on ZeroTier routes. The size stays fixed during the connection; downstream limits are not probed.") :
                              qsTr("The complete QUIC UDP payload, excluding IP and UDP headers. Must be at least 1200 bytes and fit the known interface MTU with headers and a 20-byte safety margin.")
                }

                PlankSettingLabel {
                    text: qsTr("Unreachable host timeout")
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: theme.spaceMedium

                    SpinBox {
                        id: unreachableTimeoutSpinBox
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        from: 5
                        to: 300
                        stepSize: 5
                        editable: true
                        value: StreamingPreferences.plankUnreachableTimeoutSeconds
                        onValueModified: {
                            StreamingPreferences.plankUnreachableTimeoutSeconds = value
                        }
                    }

                    Label {
                        text: qsTr("seconds")
                        color: theme.textSecondary
                        font.pointSize: 10
                        Layout.alignment: Qt.AlignVCenter
                    }
                }

                PlankSettingLabel {
                    text: qsTr("When the host remains unreachable")
                }

                AutoResizingComboBox {
                    id: unreachableActionComboBox
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    textRole: "text"
                    model: ListModel {
                        id: unreachableActionListModel
                        ListElement {
                            text: qsTr("Ask whether to disconnect or keep waiting")
                            val: StreamingPreferences.PLANK_UNREACHABLE_ASK
                        }
                        ListElement {
                            text: qsTr("Disconnect automatically")
                            val: StreamingPreferences.PLANK_UNREACHABLE_DISCONNECT
                        }
                    }
                    Component.onCompleted: {
                        currentIndex = StreamingPreferences.plankUnreachableAction === StreamingPreferences.PLANK_UNREACHABLE_DISCONNECT ? 1 : 0
                    }
                    onActivated: {
                        StreamingPreferences.plankUnreachableAction = model.get(currentIndex).val
                    }
                }

                Item {
                    Layout.preferredWidth: 280
                    Layout.preferredHeight: 1
                }

                PlankSettingHelp {
                    text: qsTr("The stream window and toolbar remain responsive while PLANK retries the host.")
                }
            }
        }

        PlankSection {
            id: advancedSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            title: qsTr("Advanced Settings")

            PlankSettingsGrid {

                PlankSettingLabel {
                    text: qsTr("Automatically find PCs on the local network")
                }

                PlankCheckBox {
                    id: enableMdns
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                    text: ""
                    Accessible.name: qsTr("Automatically find PCs on the local network")
                    checked: StreamingPreferences.enableMdns
                    enabled: !StreamingPreferences.mdnsDiscoveryManaged
                    ToolTip.visible: hovered && StreamingPreferences.mdnsDiscoveryManaged
                    ToolTip.text: qsTr("Managed by /etc/plank/client.conf")
                    onCheckedChanged: {
                        // This is called on init, so only do the work if we've
                        // actually changed the value.
                        if (StreamingPreferences.enableMdns != checked) {
                            StreamingPreferences.enableMdns = checked

                            // Restart polling so the mDNS change takes effect
                            if (window.pollingActive) {
                                ComputerManager.stopPollingAsync()
                                ComputerManager.startPolling()
                            }
                        }
                    }
                }

                PlankSettingLabel {
                    text: qsTr("Show performance stats while streaming")
                }

                PlankCheckBox {
                    id: showPerformanceOverlay
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                    text: ""
                    Accessible.name: qsTr("Show performance stats while streaming")
                    checked: StreamingPreferences.showPerformanceOverlay
                    onCheckedChanged: {
                        StreamingPreferences.showPerformanceOverlay = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Display real-time stream performance information while streaming.") + "\n\n" +
                                  qsTr("You can toggle it at any time while streaming using Ctrl+Alt+Shift+S or Select+L1+R1+X.") + "\n\n" +
                                  qsTr("The performance overlay is not supported on Steam Link or Raspberry Pi.")
                }
            }
        }
    }
}
