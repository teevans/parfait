import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: sidebar

    property int currentId: -1

    signal meetingActivated(int id)
    signal newMeetingRequested()

    function focusSearch() {
        searchField.forceActiveFocus();
        searchField.selectAll();
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.darkBackground
        Behavior on color { ColorAnimation { duration: 200 } }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // --- header ---------------------------------------------------------
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 52

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 10
                spacing: 8

                Rectangle {
                    Layout.preferredWidth: 8
                    Layout.preferredHeight: 8
                    radius: 4
                    color: Controller.recording ? Theme.red : Theme.muted
                    Behavior on color { ColorAnimation { duration: 200 } }

                    SequentialAnimation on opacity {
                        running: Controller.recording
                        loops: Animation.Infinite
                        NumberAnimation { to: 0.25; duration: 700 }
                        NumberAnimation { to: 1.0; duration: 700 }
                    }
                }

                Label {
                    text: "parfait"
                    color: Theme.foreground
                    font.family: Theme.monoFont
                    font.pixelSize: 13
                    Layout.fillWidth: true
                    Behavior on color { ColorAnimation { duration: 200 } }
                }

                ToolButton {
                    id: newButton
                    text: "+"
                    focusPolicy: Qt.NoFocus
                    implicitWidth: 28
                    implicitHeight: 28
                    onClicked: sidebar.newMeetingRequested()
                    contentItem: Label {
                        text: newButton.text
                        color: newButton.hovered ? Theme.accent : Theme.mutedForeground
                        font.family: Theme.uiFont
                        font.pixelSize: 18
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        Behavior on color { ColorAnimation { duration: 200 } }
                    }
                    background: Rectangle { color: "transparent" }
                }
            }
        }

        // --- search ---------------------------------------------------------
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 44

            TextField {
                id: searchField
                anchors.fill: parent
                anchors.margins: 8
                anchors.topMargin: 0
                placeholderText: "Search meetings  (/)"
                color: Theme.foreground
                placeholderTextColor: Theme.mutedForeground
                selectionColor: Theme.selection
                selectedTextColor: Theme.brightForeground
                font.family: Theme.uiFont
                font.pixelSize: 13
                leftPadding: 10
                onTextChanged: Meetings.filter = text
                Keys.onEscapePressed: {
                    text = "";
                    focus = false;
                }

                background: Rectangle {
                    color: Theme.background
                    border.width: 1
                    border.color: searchField.activeFocus ? Theme.accent : Theme.lighterBackground
                    Behavior on color { ColorAnimation { duration: 200 } }
                    Behavior on border.color { ColorAnimation { duration: 200 } }
                }
            }
        }

        // --- list -----------------------------------------------------------
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: Meetings
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            section.property: "dayGroup"
            section.delegate: Item {
                width: ListView.view.width
                height: 26
                Label {
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 4
                    text: section.toUpperCase()
                    color: Theme.mutedForeground
                    font.family: Theme.monoFont
                    font.pixelSize: 10
                    Behavior on color { ColorAnimation { duration: 200 } }
                }
            }

            delegate: Item {
                id: row
                required property int index
                required property var meetingId
                required property string title
                required property var startedAt
                required property string state

                width: ListView.view.width
                height: 52

                Rectangle {
                    anchors.fill: parent
                    color: row.meetingId === sidebar.currentId
                           ? Theme.lighterBackground
                           : (hover.hovered ? Theme.background : "transparent")
                    Behavior on color { ColorAnimation { duration: 200 } }

                    Rectangle {
                        width: 2
                        height: parent.height
                        color: Theme.accent
                        visible: row.meetingId === sidebar.currentId
                        Behavior on color { ColorAnimation { duration: 200 } }
                    }
                }

                HoverHandler { id: hover }

                Column {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 16
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3

                    Label {
                        width: parent.width
                        text: row.title
                        elide: Text.ElideRight
                        color: Theme.foreground
                        font.family: Theme.uiFont
                        font.pixelSize: 13
                        Behavior on color { ColorAnimation { duration: 200 } }
                    }

                    Row {
                        spacing: 8
                        Label {
                            text: Qt.formatDateTime(row.startedAt, "HH:mm")
                            color: Theme.mutedForeground
                            font.family: Theme.monoFont
                            font.pixelSize: 10
                            Behavior on color { ColorAnimation { duration: 200 } }
                        }
                        Label {
                            text: row.state
                            color: row.state === "recording" ? Theme.red : Theme.mutedForeground
                            font.family: Theme.monoFont
                            font.pixelSize: 10
                            Behavior on color { ColorAnimation { duration: 200 } }
                        }
                    }
                }

                TapHandler {
                    onTapped: sidebar.meetingActivated(row.meetingId)
                }
            }

            Label {
                anchors.centerIn: parent
                visible: list.count === 0
                text: "No meetings yet"
                color: Theme.mutedForeground
                font.family: Theme.uiFont
                font.pixelSize: 12
                Behavior on color { ColorAnimation { duration: 200 } }
            }
        }
    }

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.lighterBackground
        Behavior on color { ColorAnimation { duration: 200 } }
    }
}
