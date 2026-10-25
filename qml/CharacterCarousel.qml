import QtQuick
import QtQml.Models

Item {
    id: carousel

    property int currentIndex: 0
    property bool autoCycle: true
    property var themeRef: null

    implicitWidth: 316
    implicitHeight: 600

    readonly property int count: characterModel.count

    function next() {
        currentIndex = (currentIndex + 1) % count
    }

    function previous() {
        currentIndex = (currentIndex - 1 + count) % count
    }

    function goTo(index) {
        currentIndex = (index + count) % count
    }

    ListModel {
        id: characterModel

        ListElement {
            alias: "UCHIHA SASUKE"
            series: "火影忍者 · NARUTO"
            role: "雷遁 · 千鸟协议"
            image: "qrc:/images/sasuke.jpg"
            accent: "#A75962"
            background: "#E7E1D9"
        }
        ListElement {
            alias: "UZUMAKI NARUTO"
            series: "火影忍者 · NARUTO"
            role: "九尾查克拉 · 金色闪光"
            image: "qrc:/images/naruto.jpg"
            accent: "#B57A4D"
            background: "#E8E2D7"
        }
        ListElement {
            alias: "MONKEY D. LUFFY"
            series: "海贼王 · ONE PIECE"
            role: "橡胶果实 · 五档待机"
            image: "qrc:/images/luffy.png"
            accent: "#B26058"
            background: "#E6DCCB"
        }
        ListElement {
            alias: "RORONOA ZORO"
            series: "海贼王 · ONE PIECE"
            role: "三刀流 · 九刀流气势"
            image: "qrc:/images/zoro.jpg"
            accent: "#6E8A76"
            background: "#E7E3D9"
        }
        ListElement {
            alias: "PORTGAS D. ACE"
            series: "海贼王 · ONE PIECE"
            role: "烧烧果实 · 火拳"
            image: "qrc:/images/ace.jpg"
            accent: "#B9734E"
            background: "#E6D8C9"
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: 24
        clip: true
        color: carousel.themeRef
            ? carousel.themeRef.elevated : "#2A2D34"
        border.width: 1
        border.color: carousel.themeRef
            ? carousel.themeRef.border : "#3C4048"

        Item {
            anchors.fill: parent

            Repeater {
                model: characterModel

                delegate: Item {
                    id: page
                    required property int index
                    required property string alias
                    required property string series
                    required property string role
                    required property string image
                    required property string accent
                    required property string background

                    anchors.fill: parent
                    opacity: carousel.currentIndex === index ? 1.0 : 0.0
                    z: carousel.currentIndex === index ? 2 : 1

                    Behavior on opacity {
                        NumberAnimation {
                            duration: 720
                            easing.type: Easing.InOutQuad
                        }
                    }

                    Rectangle {
                        anchors.fill: parent
                        color: page.background
                    }

                    Image {
                        anchors.fill: parent
                        source: page.image
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        smooth: true
                        cache: true
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        height: 112
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: "#B8121317" }
                            GradientStop { position: 1.0; color: "#00121317" }
                        }
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 260
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: "#00121317" }
                            GradientStop { position: 0.45; color: "#B8121317" }
                            GradientStop { position: 1.0; color: "#F2121317" }
                        }
                    }

                    Rectangle {
                        x: 16
                        y: 16
                        width: 158
                        height: 28
                        radius: 14
                        color: "#B816171B"
                        border.width: 1
                        border.color: page.accent

                        Text {
                            anchors.centerIn: parent
                            text: String(page.index + 1).padStart(2, "0")
                                + " / " + String(carousel.count).padStart(2, "0")
                            color: carousel.themeRef
                                ? carousel.themeRef.textPrimary : "#F2EEE6"
                            font.family: carousel.themeRef
                                ? carousel.themeRef.uiFont : "Avenir Next"
                            font.pixelSize: 10
                            font.letterSpacing: 1.1
                        }
                    }

                    Column {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.leftMargin: 18
                        anchors.rightMargin: 18
                        anchors.bottomMargin: 44
                        spacing: 5

                        Text {
                            width: parent.width
                            text: page.alias
                            color: carousel.themeRef
                                ? carousel.themeRef.textPrimary : "#F2EEE6"
                            font.family: carousel.themeRef
                                ? carousel.themeRef.uiFont : "Avenir Next"
                            font.pixelSize: 24
                            font.weight: Font.Bold
                            wrapMode: Text.Wrap
                            lineHeight: 0.95
                        }

                        Text {
                            width: parent.width
                            text: page.series
                            color: carousel.themeRef
                                ? carousel.themeRef.textSecondary : "#C8C2B8"
                            font.family: carousel.themeRef
                                ? carousel.themeRef.uiFont : "Avenir Next"
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }

                        Text {
                            width: parent.width
                            text: page.role
                            color: page.accent
                            font.family: carousel.themeRef
                                ? carousel.themeRef.uiFont : "Avenir Next"
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }

                        Text {
                            width: parent.width
                            text: "LOCAL NON-COMMERCIAL FAN PROTOTYPE"
                            color: carousel.themeRef
                                ? carousel.themeRef.warning : "#C0A071"
                            font.family: carousel.themeRef
                                ? carousel.themeRef.uiFont : "Avenir Next"
                            font.pixelSize: 8
                            font.letterSpacing: 0.8
                        }
                    }
                }
            }
        }

        Rectangle {
            id: prevButton
            width: 32
            height: 32
            radius: 16
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            color: "#B316171B"
            border.width: 1
            border.color: "#71737A"

            Text {
                anchors.centerIn: parent
                text: "‹"
                color: "#F2EEE6"
                font.family: "Avenir Next"
                font.pixelSize: 22
                font.weight: Font.DemiBold
                y: -2
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: carousel.previous()
            }
        }

        Rectangle {
            id: nextButton
            width: 32
            height: 32
            radius: 16
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            color: "#B316171B"
            border.width: 1
            border.color: "#71737A"

            Text {
                anchors.centerIn: parent
                text: "›"
                color: "#F2EEE6"
                font.family: "Avenir Next"
                font.pixelSize: 22
                font.weight: Font.DemiBold
                y: -2
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: carousel.next()
            }
        }

        Row {
            visible: carousel.count <= 10
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 18
            spacing: 6

            Repeater {
                model: carousel.count

                Rectangle {
                    required property int index
                    width: carousel.currentIndex === index ? 18 : 6
                    height: 6
                    radius: 3
                    color: carousel.currentIndex === index
                        ? "#F2EEE6" : "#7B7D82"
                    opacity: carousel.currentIndex === index ? 0.95 : 0.6
                    Behavior on width {
                        NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: carousel.goTo(index)
                    }
                }
            }
        }

        Row {
            visible: carousel.count > 10
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 16
            spacing: 10

            Rectangle {
                width: 112
                height: 4
                radius: 2
                color: "#665F5F"

                Rectangle {
                    width: parent.width * (carousel.currentIndex + 1)
                        / Math.max(1, carousel.count)
                    height: parent.height
                    radius: 2
                    color: "#F2EEE6"
                }
            }

            Text {
                text: String(carousel.currentIndex + 1)
                    + " / " + String(carousel.count)
                color: "#F2EEE6"
                font.family: "Avenir Next"
                font.pixelSize: 9
            }
        }
    }

    Timer {
        interval: 6500
        running: carousel.autoCycle
        repeat: true
        onTriggered: carousel.next()
    }
}
