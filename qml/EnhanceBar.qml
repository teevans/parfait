import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shown once recording has stopped: pick a template, run the LLM merge.
Item {
    id: bar

    readonly property var templateIds: ["general", "one-on-one", "standup", "sales-call"]
    readonly property string templateId: templateIds[templateCombo.currentIndex]

    signal runRequested()

    implicitHeight: 52

    Rectangle {
        anchors.fill: parent
        color: Theme.darkBackground
        Behavior on color { ColorAnimation { duration: 200 } }

        Rectangle {
            width: parent.width
            height: 1
            color: Theme.lighterBackground
            Behavior on color { ColorAnimation { duration: 200 } }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 24
        anchors.rightMargin: 20
        spacing: 12

        Label {
            text: "Template"
            color: Theme.mutedForeground
            font.family: Theme.monoFont
            font.pixelSize: 11
            Behavior on color { ColorAnimation { duration: 200 } }
        }

        ComboBox {
            id: templateCombo
            Layout.preferredWidth: 170
            implicitHeight: 28
            model: ["General", "1-on-1", "Standup", "Sales call"]
            font.family: Theme.uiFont
            font.pixelSize: 12

            contentItem: Label {
                text: templateCombo.displayText
                color: Theme.foreground
                font: templateCombo.font
                leftPadding: 10
                rightPadding: 24
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                Behavior on color { ColorAnimation { duration: 200 } }
            }

            background: Rectangle {
                color: Theme.background
                border.width: 1
                border.color: templateCombo.hovered ? Theme.accent : Theme.lighterBackground
                Behavior on color { ColorAnimation { duration: 200 } }
                Behavior on border.color { ColorAnimation { duration: 200 } }
            }

            delegate: ItemDelegate {
                required property int index
                required property string modelData

                width: templateCombo.width
                height: 28
                highlighted: templateCombo.highlightedIndex === index

                contentItem: Label {
                    text: modelData
                    color: Theme.foreground
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    verticalAlignment: Text.AlignVCenter
                    Behavior on color { ColorAnimation { duration: 200 } }
                }
                background: Rectangle {
                    color: highlighted ? Theme.selection : Theme.darkBackground
                    Behavior on color { ColorAnimation { duration: 200 } }
                }
            }

            popup: Popup {
                y: templateCombo.height
                width: templateCombo.width
                implicitHeight: contentItem.implicitHeight
                padding: 1

                contentItem: ListView {
                    clip: true
                    implicitHeight: contentHeight
                    model: templateCombo.popup.visible ? templateCombo.delegateModel : null
                    currentIndex: templateCombo.highlightedIndex
                }

                background: Rectangle {
                    color: Theme.darkBackground
                    border.width: 1
                    border.color: Theme.lighterBackground
                    Behavior on color { ColorAnimation { duration: 200 } }
                }
            }
        }

        Item { Layout.fillWidth: true }

        Label {
            text: Enhancer.busy ? "Enhancing…"
                                : (Enhancer.configured ? "" : "No LLM endpoint (Ctrl+,)")
            color: Enhancer.busy ? Theme.accent : Theme.mutedForeground
            font.family: Theme.monoFont
            font.pixelSize: 11
            Behavior on color { ColorAnimation { duration: 200 } }
        }

        Button {
            id: runButton
            text: "Enhance"
            enabled: !Enhancer.busy && Enhancer.configured
            implicitHeight: 28
            implicitWidth: 100
            focusPolicy: Qt.NoFocus
            onClicked: bar.runRequested()

            contentItem: Label {
                text: runButton.text
                color: runButton.enabled ? Theme.background : Theme.mutedForeground
                font.family: Theme.uiFont
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                Behavior on color { ColorAnimation { duration: 200 } }
            }

            background: Rectangle {
                color: !runButton.enabled ? Theme.lighterBackground
                                          : (runButton.hovered ? Theme.blue : Theme.accent)
                Behavior on color { ColorAnimation { duration: 200 } }
            }

            ToolTip.visible: runHover.hovered
            ToolTip.text: "Merge cues + transcript (Ctrl+E)"
            HoverHandler { id: runHover }
        }
    }
}
