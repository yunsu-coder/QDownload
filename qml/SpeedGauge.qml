import QtQuick

Item {
    id: root

    property real progress: 0.0
    property bool totalKnown: true
    property color accent: "#51E7FF"
    property color secondaryAccent: "#FF78B5"
    property color trackColor: "#252B3E"
    property color textPrimary: "#F5F7FF"
    property color textMuted: "#66708B"
    property string numericFont: "DIN Alternate"
    property string uiFont: "Avenir Next"

    implicitWidth: 174
    implicitHeight: 174

    Canvas {
        id: canvas
        anchors.fill: parent

        onPaint: {
            const ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)

            const cx = width / 2
            const cy = height / 2
            const radius = Math.min(width, height) * 0.405
            const start = Math.PI * 0.75
            const end = Math.PI * 2.25

            ctx.lineCap = "round"
            ctx.lineWidth = 9
            ctx.strokeStyle = root.trackColor
            ctx.beginPath()
            ctx.arc(cx, cy, radius, start, end, false)
            ctx.stroke()

            for (let i = 0; i <= 32; ++i) {
                const angle = start + (end - start) * (i / 32)
                const inner = radius - 18
                const outer = radius - 13
                ctx.beginPath()
                ctx.moveTo(cx + Math.cos(angle) * inner,
                           cy + Math.sin(angle) * inner)
                ctx.lineTo(cx + Math.cos(angle) * outer,
                           cy + Math.sin(angle) * outer)
                ctx.lineWidth = 1
                ctx.strokeStyle = i <= Math.round(32 * root.progress)
                    ? root.accent
                    : "#32394D"
                ctx.stroke()
            }

            if (root.totalKnown && root.progress > 0.001) {
                ctx.beginPath()
                ctx.arc(cx, cy, radius, start,
                        start + (end - start) * Math.min(1, root.progress), false)
                ctx.lineWidth = 9
                ctx.strokeStyle = root.accent
                ctx.shadowColor = root.accent
                ctx.shadowBlur = 14
                ctx.stroke()
                ctx.shadowBlur = 0
            }
        }

        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
    }

    Connections {
        target: root
        function onProgressChanged() { canvas.requestPaint() }
        function onTotalKnownChanged() { canvas.requestPaint() }
    }

    Column {
        anchors.centerIn: parent
        spacing: 1

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.totalKnown
                ? Math.round(root.progress * 1000) / 10 + "%"
                : "--"
            color: root.textPrimary
            font.family: root.numericFont
            font.pixelSize: 30
            font.weight: Font.Bold
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.totalKnown ? "实际写入" : "大小未知"
            color: root.textMuted
            font.family: root.uiFont
            font.pixelSize: 9
            font.letterSpacing: 1.2
        }
    }
}
