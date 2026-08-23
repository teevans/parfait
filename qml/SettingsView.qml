import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.settings

// Minimal settings sheet. Values land in QSettings under the keys the C++ side
// reads: llm/baseUrl, llm/apiKey, llm/model, llm/fastModel, calendar/icsUrl.
// transcribe/modelPath is the exception: it is written by C++ (Models.setActive),
// so it is deliberately not mirrored into a Settings block here.
Item {
    id: settings

    signal closed()

    Settings {
        id: llm
        category: "llm"
        property string baseUrl: ""
        property string apiKey: ""
        property string model: ""
        property string fastModel: ""
    }

    Settings {
        id: calendarSettings
        category: "calendar"
        property string icsUrl: ""
    }

    component Field: ColumnLayout {
        id: field
        property string label: ""
        property string value: ""
        property string hint: ""
        property bool secret: false

        signal committed(string text)

        Layout.fillWidth: true
        Layout.fillHeight: false
        spacing: 4

        Label {
            text: field.label
            color: Theme.mutedForeground
            font.family: Theme.monoFont
            font.pixelSize: 10
            Behavior on color { ColorAnimation { duration: 200 } }
        }

        TextField {
            id: input
            Layout.fillWidth: true
            implicitHeight: 30
            text: field.value
            placeholderText: field.hint
            echoMode: field.secret ? TextInput.Password : TextInput.Normal
            color: Theme.foreground
            placeholderTextColor: Theme.mutedForeground
            selectionColor: Theme.selection
            selectedTextColor: Theme.brightForeground
            font.family: Theme.uiFont
            font.pixelSize: 12
            leftPadding: 8
            onEditingFinished: field.committed(text)

            background: Rectangle {
                color: Theme.background
                border.width: 1
                border.color: input.activeFocus ? Theme.accent : Theme.lighterBackground
                Behavior on color { ColorAnimation { duration: 200 } }
                Behavior on border.color { ColorAnimation { duration: 200 } }
            }
        }
    }

    // Flat, borderless row action — Download / Cancel / Use / Delete.
    component RowButton: Button {
        id: rowButton
        property bool danger: false

        implicitHeight: 22
        implicitWidth: Math.max(52, rowLabel.implicitWidth + 16)
        focusPolicy: Qt.NoFocus

        contentItem: Label {
            id: rowLabel
            text: rowButton.text
            color: rowButton.danger ? Theme.red
                                    : (rowButton.hovered ? Theme.background : Theme.foreground)
            font.family: Theme.uiFont
            font.pixelSize: 11
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            Behavior on color { ColorAnimation { duration: 200 } }
        }

        background: Rectangle {
            color: rowButton.hovered && !rowButton.danger ? Theme.accent : "transparent"
            border.width: 1
            border.color: rowButton.danger ? Theme.red
                                           : (rowButton.hovered ? Theme.accent : Theme.lighterBackground)
            Behavior on color { ColorAnimation { duration: 200 } }
            Behavior on border.color { ColorAnimation { duration: 200 } }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 14

        Label {
            text: "Settings"
            color: Theme.brightForeground
            font.family: Theme.uiFont
            font.pixelSize: 17
            Behavior on color { ColorAnimation { duration: 200 } }
        }

        ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true

            ColumnLayout {
                width: scroll.availableWidth
                spacing: 14

                Label {
                    text: "LLM ENDPOINT"
                    color: Theme.muted
                    font.family: Theme.monoFont
                    font.pixelSize: 10
                    Behavior on color { ColorAnimation { duration: 200 } }
                }

                Field {
                    label: "base url"
                    hint: "https://api.openai.com/v1"
                    value: llm.baseUrl
                    onCommitted: function (text) { llm.baseUrl = text; }
                }

                Field {
                    label: "api key"
                    hint: "sk-…"
                    secret: true
                    value: llm.apiKey
                    onCommitted: function (text) { llm.apiKey = text; }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: false
                    spacing: 12

                    Field {
                        label: "model"
                        hint: "gpt-4o-mini"
                        value: llm.model
                        onCommitted: function (text) { llm.model = text; }
                    }

                    Field {
                        label: "fast model (titles)"
                        hint: "optional"
                        value: llm.fastModel
                        onCommitted: function (text) { llm.fastModel = text; }
                    }
                }

                // --- transcription model ---------------------------------------
                Label {
                    Layout.topMargin: 6
                    text: "TRANSCRIPTION MODEL"
                    color: Theme.muted
                    font.family: Theme.monoFont
                    font.pixelSize: 10
                    Behavior on color { ColorAnimation { duration: 200 } }
                }

                Label {
                    Layout.fillWidth: true
                    text: "Downloaded to ~/.local/share/gromarch/models"
                    color: Theme.mutedForeground
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                    Behavior on color { ColorAnimation { duration: 200 } }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1

                    Repeater {
                        id: modelRepeater
                        model: Models.models

                        Rectangle {
                            id: modelRow
                            required property var modelData

                            // Live 0..1 progress, pushed by Models.progressChanged;
                            // the list itself is only rebuilt on state changes.
                            property real liveProgress: modelData.progress

                            Layout.fillWidth: true
                            implicitHeight: 40
                            color: modelData.active ? Theme.darkerBackground : "transparent"
                            Behavior on color { ColorAnimation { duration: 200 } }

                            Connections {
                                target: Models
                                function onProgressChanged(name, progress) {
                                    if (name === modelRow.modelData.name)
                                        modelRow.liveProgress = progress;
                                }
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 10

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1

                                    Label {
                                        Layout.fillWidth: true
                                        text: modelRow.modelData.name
                                        elide: Text.ElideRight
                                        color: modelRow.modelData.active ? Theme.accent : Theme.foreground
                                        font.family: Theme.monoFont
                                        font.pixelSize: 11
                                        Behavior on color { ColorAnimation { duration: 200 } }
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: modelRow.modelData.label + " · "
                                              + (modelRow.modelData.sizeMB >= 1024
                                                 ? (modelRow.modelData.sizeMB / 1024).toFixed(1) + " GB"
                                                 : modelRow.modelData.sizeMB + " MB")
                                              + (modelRow.modelData.downloading
                                                 ? " · " + Math.round(modelRow.liveProgress * 100) + "%"
                                                 : (modelRow.modelData.downloaded ? " · on disk" : ""))
                                        elide: Text.ElideRight
                                        color: Theme.mutedForeground
                                        font.family: Theme.uiFont
                                        font.pixelSize: 10
                                        Behavior on color { ColorAnimation { duration: 200 } }
                                    }
                                }

                                // Downloading: bar + cancel.
                                Rectangle {
                                    visible: modelRow.modelData.downloading
                                    Layout.preferredWidth: 90
                                    Layout.preferredHeight: 4
                                    color: Theme.lighterBackground
                                    Behavior on color { ColorAnimation { duration: 200 } }

                                    Rectangle {
                                        width: parent.width * Math.max(0, Math.min(1, modelRow.liveProgress))
                                        height: parent.height
                                        color: Theme.accent
                                        Behavior on width { NumberAnimation { duration: 120 } }
                                        Behavior on color { ColorAnimation { duration: 200 } }
                                    }
                                }

                                RowButton {
                                    visible: modelRow.modelData.downloading
                                    text: "Cancel"
                                    onClicked: Models.cancel(modelRow.modelData.name)
                                }

                                RowButton {
                                    visible: !modelRow.modelData.downloading && !modelRow.modelData.downloaded
                                    text: "Download"
                                    onClicked: Models.download(modelRow.modelData.name)
                                }

                                RowButton {
                                    visible: !modelRow.modelData.downloading
                                             && modelRow.modelData.downloaded
                                             && !modelRow.modelData.active
                                    text: "Use"
                                    onClicked: Models.setActive(modelRow.modelData.name)
                                }

                                RowButton {
                                    visible: !modelRow.modelData.downloading
                                             && modelRow.modelData.downloaded
                                             && !modelRow.modelData.active
                                    text: "Delete"
                                    danger: true
                                    onClicked: Models.remove(modelRow.modelData.name)
                                }

                                Label {
                                    visible: modelRow.modelData.active
                                    text: "✓ Active"
                                    color: Theme.accent
                                    font.family: Theme.uiFont
                                    font.pixelSize: 11
                                    Behavior on color { ColorAnimation { duration: 200 } }
                                }
                            }
                        }
                    }
                }

                Label {
                    id: modelError
                    Layout.fillWidth: true
                    visible: text.length > 0
                    wrapMode: Text.WordWrap
                    text: ""
                    color: Theme.red
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                    Behavior on color { ColorAnimation { duration: 200 } }

                    Connections {
                        target: Models
                        function onError(message) { modelError.text = message; }
                        function onDownloadFinished(name) { modelError.text = ""; }
                    }
                }

                Label {
                    Layout.topMargin: 6
                    text: "CALENDAR"
                    color: Theme.muted
                    font.family: Theme.monoFont
                    font.pixelSize: 10
                    Behavior on color { ColorAnimation { duration: 200 } }
                }

                Field {
                    label: "ics url"
                    hint: "https://…/basic.ics"
                    value: calendarSettings.icsUrl
                    onCommitted: function (text) { calendarSettings.icsUrl = text; }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: false

            Label {
                Layout.fillWidth: true
                text: "Stored with QSettings · nothing but text leaves this machine"
                color: Theme.mutedForeground
                font.family: Theme.uiFont
                font.pixelSize: 11
                Behavior on color { ColorAnimation { duration: 200 } }
            }

            Button {
                id: doneButton
                text: "Done"
                implicitHeight: 28
                implicitWidth: 90
                focusPolicy: Qt.NoFocus
                onClicked: settings.closed()

                contentItem: Label {
                    text: doneButton.text
                    color: Theme.background
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    Behavior on color { ColorAnimation { duration: 200 } }
                }

                background: Rectangle {
                    color: doneButton.hovered ? Theme.blue : Theme.accent
                    Behavior on color { ColorAnimation { duration: 200 } }
                }
            }
        }
    }

    Component.onCompleted: Models.refresh()
}
