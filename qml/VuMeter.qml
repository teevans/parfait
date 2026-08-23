import QtQuick
import QtQuick.Controls

// Two thin level bars: mic on top, system audio below.
Item {
    id: vu

    property real micLevel: 0
    property real systemLevel: 0
    property bool active: false

    implicitWidth: 96
    implicitHeight: 18

    Column {
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width
        spacing: 6

        Row {
            spacing: 6

            Label {
                text: "mic"
                color: Theme.mutedForeground
                font.family: Theme.monoFont
                font.pixelSize: 9
                anchors.verticalCenter: parent.verticalCenter
                Behavior on color { ColorAnimation { duration: 200 } }
            }

            Rectangle {
                width: Math.max(20, vu.width - 28)
                height: 3
                radius: 1.5
                color: Theme.lighterBackground
                anchors.verticalCenter: parent.verticalCenter
                Behavior on color { ColorAnimation { duration: 200 } }

                Rectangle {
                    height: parent.height
                    radius: parent.radius
                    width: parent.width * Math.max(0, Math.min(1, vu.micLevel))
                    color: vu.active ? Theme.accent : Theme.muted
                    Behavior on width { NumberAnimation { duration: 80 } }
                    Behavior on color { ColorAnimation { duration: 200 } }
                }
            }
        }

        Row {
            spacing: 6

            Label {
                text: "sys"
                color: Theme.mutedForeground
                font.family: Theme.monoFont
                font.pixelSize: 9
                anchors.verticalCenter: parent.verticalCenter
                Behavior on color { ColorAnimation { duration: 200 } }
            }

            Rectangle {
                width: Math.max(20, vu.width - 28)
                height: 3
                radius: 1.5
                color: Theme.lighterBackground
                anchors.verticalCenter: parent.verticalCenter
                Behavior on color { ColorAnimation { duration: 200 } }

                Rectangle {
                    height: parent.height
                    radius: parent.radius
                    width: parent.width * Math.max(0, Math.min(1, vu.systemLevel))
                    color: vu.active ? Theme.accent : Theme.muted
                    Behavior on width { NumberAnimation { duration: 80 } }
                    Behavior on color { ColorAnimation { duration: 200 } }
                }
            }
        }
    }
}
