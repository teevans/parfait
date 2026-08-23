import QtQuick
import QtQuick.Controls

Item {
    id: transcript

    // stream -> index of the trailing partial row (-1 when none)
    property var partialRow: ({ "0": -1, "1": -1 })
    property bool follow: true

    function clearAll() {
        segments.clear();
        partialRow = ({ "0": -1, "1": -1 });
        follow = true;
    }

    function rowFor(seg) {
        return {
            "stream": seg.stream,
            "t0": seg.t0,
            "t1": seg.t1,
            "text": seg.text,
            "isFinal": seg.final === true
        };
    }

    function addSegment(seg) {
        var key = String(seg.stream);
        var idx = partialRow[key] === undefined ? -1 : partialRow[key];
        var row = rowFor(seg);

        if (idx >= 0 && idx < segments.count) {
            segments.set(idx, row);
        } else {
            segments.append(row);
            idx = segments.count - 1;
        }

        var next = partialRow;
        next[key] = row.isFinal ? -1 : idx;
        partialRow = next;

        if (follow)
            listView.positionViewAtEnd();
    }

    function setSegments(list) {
        clearAll();
        for (var i = 0; i < list.length; ++i) {
            segments.append({
                "stream": list[i].stream,
                "t0": list[i].t0,
                "t1": list[i].t1,
                "text": list[i].text,
                "isFinal": true
            });
        }
        listView.positionViewAtEnd();
    }

    function stamp(seconds) {
        var s = Math.max(0, Math.floor(seconds));
        var m = Math.floor(s / 60);
        var r = s % 60;
        return (m < 10 ? "0" : "") + m + ":" + (r < 10 ? "0" : "") + r;
    }

    ListModel { id: segments }

    ListView {
        id: listView
        anchors.fill: parent
        anchors.margins: 4
        clip: true
        model: segments
        spacing: 10
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { id: bar }

        onContentYChanged: {
            if (!listView.moving)
                return;
            transcript.follow = listView.atYEnd;
        }
        onMovementEnded: transcript.follow = listView.atYEnd

        delegate: Column {
            id: segDelegate
            required property var model

            width: ListView.view.width - 16
            x: 8
            spacing: 2
            opacity: segDelegate.model.isFinal ? 1.0 : 0.65
            Behavior on opacity { NumberAnimation { duration: 150 } }

            Row {
                spacing: 8
                Label {
                    text: segDelegate.model.stream === 0 ? "Me" : "Them"
                    color: segDelegate.model.stream === 0 ? Theme.accent : Theme.muted
                    font.family: Theme.monoFont
                    font.pixelSize: 11
                    Behavior on color { ColorAnimation { duration: 200 } }
                }
                Label {
                    text: transcript.stamp(segDelegate.model.t0)
                    color: Theme.mutedForeground
                    font.family: Theme.monoFont
                    font.pixelSize: 10
                    anchors.verticalCenter: parent.verticalCenter
                    Behavior on color { ColorAnimation { duration: 200 } }
                }
            }

            Label {
                width: parent.width
                text: segDelegate.model.text
                wrapMode: Text.WordWrap
                color: Theme.foreground
                font.family: Theme.uiFont
                font.pixelSize: 13
                lineHeight: 1.25
                Behavior on color { ColorAnimation { duration: 200 } }
            }
        }
    }

    Label {
        anchors.centerIn: parent
        visible: segments.count === 0
        text: Controller.recording ? "Listening…" : "No transcript yet"
        color: Theme.mutedForeground
        font.family: Theme.uiFont
        font.pixelSize: 12
        Behavior on color { ColorAnimation { duration: 200 } }
    }

    // "jump back to live" affordance while scroll-locked
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 10
        visible: !transcript.follow && segments.count > 0
        width: followLabel.implicitWidth + 20
        height: 24
        radius: 12
        color: Theme.lighterBackground
        border.width: 1
        border.color: Theme.accent
        Behavior on color { ColorAnimation { duration: 200 } }

        Label {
            id: followLabel
            anchors.centerIn: parent
            text: "Jump to live"
            color: Theme.foreground
            font.family: Theme.uiFont
            font.pixelSize: 11
            Behavior on color { ColorAnimation { duration: 200 } }
        }

        TapHandler {
            onTapped: {
                transcript.follow = true;
                listView.positionViewAtEnd();
            }
        }
    }
}
