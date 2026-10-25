import QtQuick

Item {
    id: root

    property var samples: []
    property color lineColor: "#51E7FF"
    property color fillColor: "#1451E7FF"
    property color gridColor: "#20263A"
    property color textColor: "#66708B"
    property string uiFont: "Avenir Next"

    Canvas {
        id: canvas
        anchors.fill: parent
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()

        onPaint: {
            const ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)

            const left = 8
            const right = width - 8
            const top = 8
            const bottom = height - 8
            const chartWidth = right - left
            const chartHeight = bottom - top

            ctx.lineWidth = 1
            ctx.strokeStyle = root.gridColor
            for (let i = 0; i <= 3; ++i) {
                const y = top + chartHeight * i / 3
                ctx.beginPath()
                ctx.moveTo(left, y)
                ctx.lineTo(right, y)
                ctx.stroke()
            }

            if (!root.samples || root.samples.length < 2) {
                return
            }

            let maxValue = 1
            for (let i = 0; i < root.samples.length; ++i) {
                maxValue = Math.max(maxValue, Number(root.samples[i]))
            }
            maxValue *= 1.18

            const pointAt = function(index) {
                const x = left + chartWidth * index / (root.samples.length - 1)
                const value = Math.max(0, Number(root.samples[index]))
                const y = bottom - chartHeight * value / maxValue
                return [x, y]
            }

            const area = ctx.createLinearGradient(0, top, 0, bottom)
            area.addColorStop(0, root.fillColor)
            area.addColorStop(1, "rgba(81,231,255,0)")

            ctx.beginPath()
            let p = pointAt(0)
            ctx.moveTo(p[0], bottom)
            ctx.lineTo(p[0], p[1])
            for (let i = 1; i < root.samples.length; ++i) {
                p = pointAt(i)
                ctx.lineTo(p[0], p[1])
            }
            ctx.lineTo(right, bottom)
            ctx.closePath()
            ctx.fillStyle = area
            ctx.fill()

            ctx.beginPath()
            p = pointAt(0)
            ctx.moveTo(p[0], p[1])
            for (let i = 1; i < root.samples.length; ++i) {
                p = pointAt(i)
                ctx.lineTo(p[0], p[1])
            }
            ctx.lineWidth = 2
            ctx.strokeStyle = root.lineColor
            ctx.shadowColor = root.lineColor
            ctx.shadowBlur = 8
            ctx.stroke()
            ctx.shadowBlur = 0

            p = pointAt(root.samples.length - 1)
            ctx.beginPath()
            ctx.arc(p[0], p[1], 3.2, 0, Math.PI * 2)
            ctx.fillStyle = root.lineColor
            ctx.fill()
        }
    }

    Connections {
        target: root
        function onSamplesChanged() { canvas.requestPaint() }
    }
}
