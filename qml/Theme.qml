import QtQuick

QtObject {
    readonly property color canvas: "#F3F8FF"
    readonly property color panel: "#FFFFFF"
    readonly property color card: "#FFFFFF"
    readonly property color elevated: "#EAF2FC"
    readonly property color border: "#D3E3F5"
    readonly property color textPrimary: "#142B4A"
    readonly property color textSecondary: "#476687"
    readonly property color textMuted: "#7890AC"
    readonly property color cyan: "#2F73E8"
    readonly property color indigo: "#4E6FBC"
    readonly property color crimson: "#E45C6C"
    readonly property color pink: "#7C9DEB"
    readonly property color success: "#2A9D76"
    readonly property color warning: "#B7791F"
    readonly property color danger: "#D4404E"

    readonly property string uiFont: Qt.platform.os === "osx"
        ? "Avenir Next"
        : (Qt.platform.os === "windows" ? "Segoe UI" : "Noto Sans")
    readonly property string numericFont: Qt.platform.os === "osx"
        ? "DIN Alternate"
        : (Qt.platform.os === "windows" ? "Bahnschrift" : "DejaVu Sans")
}
