import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: empty

    signal startRequested()

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 80, 420)
        spacing: 14

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: "parfait"
            color: Theme.foreground
            font.family: Theme.monoFont
            font.pixelSize: 22
            Behavior on color { ColorAnimation { duration: 200 } }
        }

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: "Record the call, type rough cues while it happens, and let the model merge them into a real note afterwards."
            color: Theme.mutedForeground
            font.family: Theme.uiFont
            font.pixelSize: 13
            Behavior on color { ColorAnimation { duration: 200 } }
        }

        Button {
            id: startButton
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 6
            text: "Start recording"
            implicitHeight: 34
            implicitWidth: 160
            focusPolicy: Qt.NoFocus
            onClicked: empty.startRequested()

            contentItem: Label {
                text: startButton.text
                color: Theme.background
                font.family: Theme.uiFont
                font.pixelSize: 13
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                Behavior on color { ColorAnimation { duration: 200 } }
            }

            background: Rectangle {
                color: startButton.hovered ? Theme.blue : Theme.accent
                Behavior on color { ColorAnimation { duration: 200 } }
            }
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: "Ctrl+R record   ·   Ctrl+E enhance   ·   /  search   ·   Ctrl+, settings"
            color: Theme.muted
            font.family: Theme.monoFont
            font.pixelSize: 10
            Behavior on color { ColorAnimation { duration: 200 } }
        }
    }
}
