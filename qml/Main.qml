import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root

    property int selectedId: -1
    // "/" must not steal the key while the user is typing in a text field.
    readonly property bool typing: root.activeFocusItem !== null
                                   && root.activeFocusItem.hasOwnProperty("selectedText")

    visible: true
    width: 1200
    height: 800
    minimumWidth: 780
    minimumHeight: 480
    title: "Gromarch"
    color: Theme.background

    Behavior on color { ColorAnimation { duration: 200 } }

    function selectMeeting(id) {
        root.selectedId = id;
        meetingView.reload();
    }

    function toggleRecord() {
        if (Controller.recording) {
            Controller.stopMeeting();
            meetingView.reload();
        } else {
            var id = Controller.startMeeting("");
            if (id >= 0) {
                meetingView.resetTranscript();
                root.selectMeeting(id);
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Sidebar {
            id: sidebar
            Layout.fillHeight: true
            Layout.preferredWidth: 268
            currentId: root.selectedId
            onMeetingActivated: function (id) { root.selectMeeting(id); }
            onNewMeetingRequested: root.toggleRecord()
        }

        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            color: Theme.lighterBackground
            Behavior on color { ColorAnimation { duration: 200 } }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            MeetingView {
                id: meetingView
                anchors.fill: parent
                meetingId: root.selectedId
                visible: root.selectedId >= 0
            }

            EmptyState {
                anchors.fill: parent
                visible: root.selectedId < 0
                onStartRequested: root.toggleRecord()
            }
        }
    }

    // --- notifications ------------------------------------------------------
    Rectangle {
        id: toast
        property string message: ""

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        width: Math.min(toastLabel.implicitWidth + 32, root.width - 80)
        height: 40
        radius: 4
        visible: opacity > 0
        opacity: 0
        color: Theme.darkerBackground
        border.width: 1
        border.color: Theme.red

        Behavior on opacity { NumberAnimation { duration: 200 } }
        Behavior on color { ColorAnimation { duration: 200 } }

        Label {
            id: toastLabel
            anchors.centerIn: parent
            text: toast.message
            elide: Text.ElideRight
            width: parent.width - 32
            horizontalAlignment: Text.AlignHCenter
            color: Theme.foreground
            font.family: Theme.uiFont
            font.pixelSize: 13
            Behavior on color { ColorAnimation { duration: 200 } }
        }

        Timer {
            id: toastTimer
            interval: 4000
            onTriggered: toast.opacity = 0
        }

        function show(text) {
            toast.message = text;
            toast.opacity = 1;
            toastTimer.restart();
        }
    }

    Connections {
        target: Controller

        function onLiveSegment(segment) {
            meetingView.addSegment(segment);
        }
        function onEnhanceDelta(meetingId, textDelta) {
            if (meetingId === root.selectedId)
                meetingView.appendEnhanceDelta(textDelta);
        }
        function onEnhanceFinished(meetingId) {
            if (meetingId === root.selectedId)
                meetingView.enhanceFinished();
        }
        function onError(message) {
            toast.show(message);
        }
        function onStateChanged() {
            if (!Controller.recording)
                meetingView.reload();
        }
    }

    // --- settings -----------------------------------------------------------
    Popup {
        id: settingsPopup
        anchors.centerIn: Overlay.overlay
        width: Math.min(560, root.width - 80)
        height: Math.min(460, root.height - 80)
        modal: true
        padding: 0

        background: Rectangle {
            color: Theme.darkBackground
            border.width: 1
            border.color: Theme.lighterBackground
            Behavior on color { ColorAnimation { duration: 200 } }
        }

        // Loaded by URL (not as an inline component) so that a missing optional
        // QML module degrades to this one panel instead of the whole app.
        Loader {
            id: settingsLoader
            anchors.fill: parent
            active: settingsPopup.opened
            source: "SettingsView.qml"
            onLoaded: item.closed.connect(settingsPopup.close)
        }

        Label {
            anchors.centerIn: parent
            width: parent.width - 48
            visible: settingsLoader.status === Loader.Error
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            text: "Settings are unavailable — install the Qt Labs Settings QML module (qml6-module-qt-labs-settings)."
            color: Theme.mutedForeground
            font.family: Theme.uiFont
            font.pixelSize: 12
            Behavior on color { ColorAnimation { duration: 200 } }
        }
    }

    // --- shortcuts ----------------------------------------------------------
    Shortcut {
        sequence: "Ctrl+R"
        onActivated: root.toggleRecord()
    }
    Shortcut {
        sequence: "Ctrl+E"
        onActivated: if (root.selectedId >= 0) meetingView.runEnhance()
    }
    Shortcut {
        sequence: "Ctrl+,"
        onActivated: settingsPopup.open()
    }
    Shortcut {
        sequence: "/"
        enabled: !root.typing
        onActivated: sidebar.focusSearch()
    }
}
