import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: control

    property var themeRef: null
    property string directoryText: ""
    property string proxyLabel: ""
    property color proxyAccent: "#2F73E8"
    property string routeText: ""
    property string routeNoteText: ""
    property string memoryHint: ""
    property string connectionLimitText: ""
    property bool directoryEditable: true
    property bool connectionEditable: true
    property bool settingsEditable: true

    signal changeDirectoryRequested()
    signal openDirectoryRequested()
    signal cycleProxyRequested()
    signal cycleConnectionLimitRequested()
    signal manageProxyRequested()
    signal clearMemoryRequested()

    modal: true
    width: 500
    height: 262
    anchors.centerIn: Overlay.overlay
    padding: 0
    closePolicy: Popup.CloseOnEscape

    background: Rectangle {
        radius: 24
        color: control.themeRef.card
        border.width: 1
        border.color: control.themeRef.border
    }

    contentItem: ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        RowLayout {
            Layout.fillWidth: true

            Text {
                text: "设置"
                color: control.themeRef.textPrimary
                font.family: control.themeRef.uiFont
                font.pixelSize: 18
                font.weight: Font.Bold
            }

            Item { Layout.fillWidth: true }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 60
            radius: 14
            color: control.themeRef.elevated
            border.width: 1
            border.color: control.themeRef.border

            RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 12

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 3

                    Text {
                        text: "保存位置"
                        color: control.themeRef.textSecondary
                        font.family: control.themeRef.uiFont
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                    }

                    Text {
                        Layout.fillWidth: true
                        text: control.directoryText
                        color: control.themeRef.textMuted
                        font.family: control.themeRef.uiFont
                        font.pixelSize: 10
                        elide: Text.ElideMiddle
                    }
                }

                AppButton {
                    text: "打开"
                    Layout.preferredWidth: 58
                    Layout.preferredHeight: 30
                    accent: control.themeRef.cyan
                    panelColor: control.themeRef.card
                    borderColor: control.themeRef.border
                    uiFont: control.themeRef.uiFont
                    numericFont: control.themeRef.numericFont
                    onClicked: control.openDirectoryRequested()
                }

                AppButton {
                    text: "更改"
                    Layout.preferredWidth: 62
                    Layout.preferredHeight: 30
                    accent: control.themeRef.cyan
                    panelColor: control.themeRef.card
                    borderColor: control.themeRef.border
                    uiFont: control.themeRef.uiFont
                    numericFont: control.themeRef.numericFont
                    enabled: control.directoryEditable
                    onClicked: control.changeDirectoryRequested()
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 92
            radius: 14
            color: control.themeRef.elevated
            border.width: 1
            border.color: control.themeRef.border

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 5

                RowLayout {
                    Layout.fillWidth: true

                    Text {
                        text: "代理"
                        color: control.themeRef.textSecondary
                        font.family: control.themeRef.uiFont
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                    }

                    Item { Layout.fillWidth: true }

                    Rectangle {
                        id: proxyPill
                        Layout.preferredWidth: 122
                        Layout.preferredHeight: 22
                        radius: 11
                        color: "#EAF3FF"
                        border.width: 1
                        border.color: control.proxyAccent

                        Text {
                            anchors.centerIn: parent
                            text: control.proxyLabel
                            color: control.proxyAccent
                            font.family: control.themeRef.uiFont
                            font.pixelSize: 8
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.6
                        }

                        MouseArea {
                            id: proxyHover
                            anchors.fill: parent
                            hoverEnabled: true
                        }

                        ToolTip.visible: proxyHover.containsMouse
                        ToolTip.text: control.routeText
                            + (control.routeNoteText.length > 0
                                ? "\n" + control.routeNoteText : "")
                            + (control.memoryHint.length > 0
                                ? "\n" + control.memoryHint : "")
                        ToolTip.delay: 350
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    AppButton {
                        text: "切换路由"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 30
                        accent: control.themeRef.cyan
                        panelColor: control.themeRef.card
                        borderColor: control.themeRef.border
                        uiFont: control.themeRef.uiFont
                        numericFont: control.themeRef.numericFont
                        enabled: control.settingsEditable
                        onClicked: control.cycleProxyRequested()
                    }

                    AppButton {
                        text: "内置代理"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 28
                        accent: control.themeRef.cyan
                        panelColor: control.themeRef.card
                        borderColor: control.themeRef.border
                        uiFont: control.themeRef.uiFont
                        numericFont: control.themeRef.numericFont
                        enabled: control.settingsEditable
                        onClicked: control.manageProxyRequested()
                    }

                    AppButton {
                        text: control.connectionLimitText
                        Layout.preferredWidth: 88
                        Layout.preferredHeight: 28
                        accent: control.themeRef.cyan
                        panelColor: control.themeRef.card
                        borderColor: control.themeRef.border
                        uiFont: control.themeRef.uiFont
                        numericFont: control.themeRef.numericFont
                        enabled: control.connectionEditable
                        onClicked: control.cycleConnectionLimitRequested()
                    }
                }
            }
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }

            AppButton {
                text: "清记忆"
                Layout.preferredWidth: 72
                Layout.preferredHeight: 30
                accent: control.themeRef.warning
                panelColor: control.themeRef.elevated
                borderColor: control.themeRef.border
                uiFont: control.themeRef.uiFont
                numericFont: control.themeRef.numericFont
                onClicked: control.clearMemoryRequested()
            }

            AppButton {
                text: "完成"
                primary: true
                Layout.preferredWidth: 76
                Layout.preferredHeight: 30
                accent: control.themeRef.cyan
                panelColor: control.themeRef.elevated
                borderColor: control.themeRef.border
                uiFont: control.themeRef.uiFont
                numericFont: control.themeRef.numericFont
                onClicked: control.close()
            }
        }
    }
}
