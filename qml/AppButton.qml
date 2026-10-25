import QtQuick
import QtQuick.Controls

Button {
    id: control

    property bool primary: false
    property color accent: "#2F73E8"
    property color primaryColor: "#2F73E8"
    property color primaryHoverColor: "#4385F5"
    property color panelColor: "#EAF2FC"
    property color borderColor: "#C9DCF4"
    property color textColor: "#1A3455"
    property string uiFont: "Avenir Next"
    property string numericFont: "DIN Alternate"

    implicitHeight: 44
    implicitWidth: primary ? 152 : 122
    hoverEnabled: true
    opacity: enabled ? 1.0 : 0.42
    scale: pressed ? 0.98 : (hovered ? 1.01 : 1.0)
    font.family: uiFont

    Behavior on scale {
        NumberAnimation { duration: 110; easing.type: Easing.OutCubic }
    }

    background: Rectangle {
        radius: 14
        border.width: 1
        color: control.primary
            ? (control.pressed ? "#1E5AC6"
                : (control.hovered ? control.primaryHoverColor : control.primaryColor))
            : (control.hovered ? "#F7FAFF" : control.panelColor)
        border.color: control.primary
            ? "#4C8EF5"
            : (control.hovered ? control.accent : control.borderColor)
    }

    contentItem: Text {
        text: control.text
        color: control.primary ? "#FFFFFF" : control.textColor
        font.family: control.uiFont
        font.pixelSize: 13
        font.weight: Font.DemiBold
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
