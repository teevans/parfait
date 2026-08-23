import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The enhanced note: rendered Markdown by default, raw source on toggle.
Item {
    id: note

    property string markdown: ""
    property bool raw: false

    function setText(text) {
        note.markdown = text === undefined ? "" : text;
    }

    function appendText(delta) {
        note.markdown += delta;
        var inner = flick.contentItem;   // ScrollView's Flickable
        if (inner)
            inner.contentY = Math.max(0, inner.contentHeight - flick.height);
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ScrollView {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            TextArea {
                id: area
                text: note.markdown
                readOnly: !note.raw
                textFormat: note.raw ? TextEdit.PlainText : TextEdit.MarkdownText
                wrapMode: TextArea.Wrap
                selectByMouse: true
                leftPadding: 18
                rightPadding: 18
                topPadding: 14
                bottomPadding: 20
                color: Theme.foreground
                selectionColor: Theme.selection
                selectedTextColor: Theme.brightForeground
                font.family: note.raw ? Theme.monoFont : Theme.uiFont
                font.pixelSize: 13
                background: Rectangle { color: "transparent" }
                Behavior on color { ColorAnimation { duration: 200 } }

                onLinkActivated: function (link) { Qt.openUrlExternally(link); }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: 26
            Layout.maximumHeight: 26
            Layout.leftMargin: 18
            Layout.rightMargin: 12
            spacing: 10

            Label {
                text: note.markdown.length === 0 ? "Nothing enhanced yet" : ""
                color: Theme.mutedForeground
                font.family: Theme.uiFont
                font.pixelSize: 11
                Layout.fillWidth: true
                Behavior on color { ColorAnimation { duration: 200 } }
            }

            Label {
                text: note.raw ? "rendered" : "raw"
                color: rawHover.hovered ? Theme.accent : Theme.mutedForeground
                font.family: Theme.monoFont
                font.pixelSize: 10
                Behavior on color { ColorAnimation { duration: 200 } }

                HoverHandler { id: rawHover }
                TapHandler { onTapped: note.raw = !note.raw }
            }
        }
    }
}
