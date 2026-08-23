import QtQuick
import QtQuick.Controls

// Rough-cue editor. Debounced autosave (800 ms) back into the Library.
Item {
    id: notepad

    property int meetingId: -1
    property bool loading: false

    function setText(text) {
        loading = true;
        saveTimer.stop();
        area.text = text === undefined ? "" : text;
        loading = false;
    }

    function flush() {
        saveTimer.stop();
        if (notepad.meetingId >= 0)
            Controller.saveCues(notepad.meetingId, area.text);
    }

    Timer {
        id: saveTimer
        interval: 800
        onTriggered: notepad.flush()
    }

    ScrollView {
        anchors.fill: parent
        clip: true

        TextArea {
            id: area
            placeholderText: "Type rough cues — fragments are fine…"
            wrapMode: TextArea.Wrap
            selectByMouse: true
            leftPadding: 24
            rightPadding: 24
            topPadding: 18
            bottomPadding: 24
            color: Theme.foreground
            placeholderTextColor: Theme.mutedForeground
            selectionColor: Theme.selection
            selectedTextColor: Theme.brightForeground
            font.family: Theme.uiFont
            font.pixelSize: 14
            background: Rectangle {
                color: "transparent"
            }

            Behavior on color { ColorAnimation { duration: 200 } }

            onTextChanged: {
                if (!notepad.loading)
                    saveTimer.restart();
            }
        }
    }
}
