import QtQuick

Item {
    id: root

    property int active: 0
    property int total: 16
    property int pulse: 0
    property color activeColor: "#51E7FF"
    property color idleColor: "#30374C"

    implicitHeight: 28

    Row {
        anchors.fill: parent
        spacing: 4

        Repeater {
            model: root.total

            Rectangle {
                required property int index

                width: Math.max(3, (root.width - (root.total - 1) * 4) / root.total)
                height: index < root.active
                    ? 9 + ((index * 7 + root.pulse * 3) % 15)
                    : 4
                anchors.bottom: parent.bottom
                radius: 3
                color: index < root.active ? root.activeColor : root.idleColor
                opacity: index < root.active ? 0.95 : 0.62

                Behavior on height {
                    NumberAnimation {
                        duration: 280
                        easing.type: Easing.OutCubic
                    }
                }
            }
        }
    }
}
