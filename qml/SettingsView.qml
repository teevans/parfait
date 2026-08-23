import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.settings

// Minimal settings sheet. Values land in QSettings under the keys the C++ side
// reads: llm/baseUrl, llm/apiKey, llm/model, llm/fastModel, calendar/icsUrl.
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

        Item { Layout.fillHeight: true }

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
}
