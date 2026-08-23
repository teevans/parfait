import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: view

    property int meetingId: -1
    property var info: ({})
    property bool live: Controller.recording && Controller.currentMeetingId === view.meetingId

    onMeetingIdChanged: reload()

    function reload() {
        if (view.meetingId < 0) {
            view.info = ({});
            return;
        }
        view.info = Meetings.meetingById(view.meetingId);
        titleField.text = view.info.title === undefined ? "" : view.info.title;
        notepad.setText(view.info.notesMd);
        note.setText(view.info.enhancedMd === undefined ? "" : view.info.enhancedMd);
        if (!view.live)
            transcript.setSegments(Meetings.segmentsFor(view.meetingId));
    }

    function resetTranscript() {
        transcript.clearAll();
        note.setText("");
    }

    function addSegment(segment) {
        transcript.addSegment(segment);
    }

    function appendEnhanceDelta(delta) {
        note.appendText(delta);
    }

    function enhanceFinished() {
        view.reload();
        rightStack.currentIndex = 1;
    }

    function runEnhance() {
        if (view.meetingId < 0 || Controller.recording)
            return;
        notepad.flush();
        note.setText("");
        rightStack.currentIndex = 1;
        Controller.enhance(view.meetingId, enhanceBar.templateId);
    }

    function clock(seconds) {
        var s = Math.max(0, Math.floor(seconds));
        var h = Math.floor(s / 3600);
        var m = Math.floor((s % 3600) / 60);
        var r = s % 60;
        var mm = (m < 10 ? "0" : "") + m;
        var ss = (r < 10 ? "0" : "") + r;
        return h > 0 ? h + ":" + mm + ":" + ss : mm + ":" + ss;
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // --- header -----------------------------------------------------------
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 64

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 24
                anchors.rightMargin: 20
                spacing: 16

                TextField {
                    id: titleField
                    Layout.fillWidth: true
                    placeholderText: "Untitled meeting"
                    color: Theme.brightForeground
                    placeholderTextColor: Theme.mutedForeground
                    selectionColor: Theme.selection
                    selectedTextColor: Theme.brightForeground
                    font.family: Theme.uiFont
                    font.pixelSize: 18
                    leftPadding: 0
                    background: Rectangle { color: "transparent" }
                    Behavior on color { ColorAnimation { duration: 200 } }

                    onEditingFinished: {
                        if (view.meetingId >= 0)
                            Controller.setTitle(view.meetingId, text);
                    }
                }

                Label {
                    text: view.clock(view.live ? Controller.elapsedSeconds : 0)
                    visible: view.live
                    color: Theme.foreground
                    font.family: Theme.monoFont
                    font.pixelSize: 15
                    Behavior on color { ColorAnimation { duration: 200 } }
                }

                Label {
                    text: view.info.startedAt === undefined
                          ? "" : Qt.formatDateTime(view.info.startedAt, "ddd d MMM · HH:mm")
                    visible: !view.live
                    color: Theme.mutedForeground
                    font.family: Theme.monoFont
                    font.pixelSize: 11
                    Behavior on color { ColorAnimation { duration: 200 } }
                }

                VuMeter {
                    Layout.preferredWidth: 110
                    micLevel: Audio.micLevel
                    systemLevel: Audio.systemLevel
                    active: Controller.recording
                }

                // record / stop
                Rectangle {
                    Layout.preferredWidth: 34
                    Layout.preferredHeight: 34
                    radius: 17
                    color: "transparent"
                    border.width: 1
                    border.color: Controller.recording ? Theme.red : Theme.lighterBackground
                    Behavior on border.color { ColorAnimation { duration: 200 } }

                    Rectangle {
                        anchors.centerIn: parent
                        width: Controller.recording ? 12 : 18
                        height: width
                        radius: Controller.recording ? 2 : width / 2
                        color: Controller.recording ? Theme.red : Theme.accent
                        Behavior on width { NumberAnimation { duration: 150 } }
                        Behavior on radius { NumberAnimation { duration: 150 } }
                        Behavior on color { ColorAnimation { duration: 200 } }
                    }

                    TapHandler {
                        onTapped: {
                            notepad.flush();
                            if (Controller.recording)
                                Controller.stopMeeting();
                            else
                                Controller.startMeeting(titleField.text);
                        }
                    }

                    ToolTip.visible: hoverHandler.hovered
                    ToolTip.text: Controller.recording ? "Stop (Ctrl+R)" : "Record (Ctrl+R)"
                    HoverHandler { id: hoverHandler }
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.lighterBackground
                Behavior on color { ColorAnimation { duration: 200 } }
            }
        }

        // --- panes ------------------------------------------------------------
        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal

            handle: Rectangle {
                implicitWidth: 5
                color: SplitHandle.pressed || SplitHandle.hovered
                       ? Theme.accent : Theme.lighterBackground
                Behavior on color { ColorAnimation { duration: 200 } }
            }

            NotePad {
                id: notepad
                SplitView.fillWidth: true
                SplitView.minimumWidth: 280
                meetingId: view.meetingId
            }

            ColumnLayout {
                SplitView.preferredWidth: 420
                SplitView.minimumWidth: 260
                spacing: 0

                // transcript / enhanced note switch
                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: false
                    Layout.preferredHeight: 34
                    Layout.maximumHeight: 34
                    spacing: 0

                    Repeater {
                        model: ["Transcript", "Note"]

                        Item {
                            required property int index
                            required property string modelData

                            Layout.fillWidth: true
                            Layout.fillHeight: true

                            Rectangle {
                                anchors.fill: parent
                                color: "transparent"
                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    width: parent.width
                                    height: 1
                                    color: rightStack.currentIndex === index
                                           ? Theme.accent : Theme.lighterBackground
                                    Behavior on color { ColorAnimation { duration: 200 } }
                                }
                            }

                            Label {
                                anchors.centerIn: parent
                                text: modelData
                                color: rightStack.currentIndex === index
                                       ? Theme.foreground : Theme.mutedForeground
                                font.family: Theme.monoFont
                                font.pixelSize: 11
                                Behavior on color { ColorAnimation { duration: 200 } }
                            }

                            TapHandler {
                                onTapped: rightStack.currentIndex = index
                            }
                        }
                    }
                }

                StackLayout {
                    id: rightStack
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: 0

                    TranscriptView { id: transcript }

                    EnhancedNote { id: note }
                }
            }
        }

        // --- enhance bar --------------------------------------------------------
        EnhanceBar {
            id: enhanceBar
            Layout.fillWidth: true
            visible: !Controller.recording && view.meetingId >= 0
            onRunRequested: view.runEnhance()
        }
    }
}
