import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ApplicationWindow {
    id: root

    width: 600
    height: hlsDialog.visible
        ? 420
        : (settingsDialog.visible || builtinDialog.visible || confirmDialog.visible
            ? 280 : (downloadController.hasTask ? 238 : 118))
    minimumWidth: 600
    minimumHeight: 118
    visible: true
    title: "快下载"
    color: theme.canvas

    Theme { id: theme }

    property string toastText: ""
    property bool toastVisible: false
    property string pendingDiscardAction: ""
    property string pendingDiscardUrl: ""

    function formatBytes(bytes) {
        if (!bytes || bytes <= 0)
            return "0 B"
        const units = ["B", "KB", "MB", "GB", "TB"]
        let value = Number(bytes)
        let index = 0
        while (value >= 1024 && index < units.length - 1) {
            value /= 1024
            index++
        }
        return value.toFixed(index === 0 ? 0 : (value >= 100 ? 0 : 1)) + " " + units[index]
    }

    function etaText() {
        if (downloadController.protocolName === "HLS"
                && downloadController.totalSegments > 0)
            return downloadController.completedSegments + " / "
                + downloadController.totalSegments + " 分片"
        if (downloadController.state !== "Downloading"
                || downloadController.etaSeconds <= 0)
            return "剩余时间 --"
        const seconds = downloadController.etaSeconds
        const minutes = Math.floor(seconds / 60)
        const remain = seconds % 60
        return "剩余 " + String(minutes).padStart(2, "0") + ":" + String(remain).padStart(2, "0")
    }

    function taskStatusText() {
        switch (downloadController.state) {
        case "Selecting": return "选择清晰度"
        case "Probing": return "探测中"
        case "Completing": return "校验中"
        case "Paused": return "已暂停"
        case "Failed": return "下载失败"
        case "Completed": return "已完成"
        default: return "等待中"
        }
    }

    function progressValue() {
        if (downloadController.protocolName === "HLS"
                && downloadController.totalSegments > 0)
            return Math.min(1.0, downloadController.completedSegments
                / downloadController.totalSegments)
        return downloadController.progress
    }

    function proxyModeLabel() {
        if (!downloadController.proxyConfigured)
            return downloadController.proxyMode === "direct" ? "仅直连" : "未配置"
        switch (downloadController.proxyMode) {
        case "builtin": return "内置 Clash"
        case "proxy": return "仅 iKuuu"
        case "direct": return "仅直连"
        default: return downloadController.embeddedProxyConfigured
            ? "自动 · 内置" : "自动 · iKuuu"
        }
    }

    function proxyModeColor() {
        if (!downloadController.proxyConfigured)
            return theme.warning
        return downloadController.proxyMode === "direct"
            ? theme.textMuted : theme.success
    }

    function cycleProxyMode() {
        if (downloadController.state === "Downloading"
                || downloadController.state === "Probing") {
            showToast("下载过程中不能切换路由模式")
            return
        }
        downloadController.cycleProxyMode()
        showToast("路由模式已切换：" + proxyModeLabel())
    }

    function hasNewUrl() {
        const text = urlField.text.trim()
        return text.length > 0 && text !== downloadController.url
            && downloadController.state !== "Downloading"
            && downloadController.state !== "Probing"
            && downloadController.state !== "Completing"
    }

    function primaryActionText() {
        if (!downloadController.hasTask || hasNewUrl())
            return "开始"
        switch (downloadController.state) {
        case "Downloading":
        case "Probing": return "暂停"
        case "Paused": return "继续"
        case "Selecting": return "选择清晰度"
        case "Failed": return downloadController.sourceChanged
            ? "重新下载" : "重试"
        case "Completing": return "校验中"
        case "Completed": return "打开"
        default: return "开始"
        }
    }

    function transferActive() {
        return downloadController.state === "Downloading"
            || downloadController.state === "Probing"
            || downloadController.state === "Selecting"
            || downloadController.state === "Paused"
            || downloadController.state === "Completing"
    }

    function pasteClipboard(showMissing) {
        const value = downloadController.clipboardUrl()
        if (value.length > 0) {
            urlField.text = value
            prewarmTimer.restart()
            return
        }
        if (showMissing)
            showToast("剪贴板里没有下载链接")
    }

    function cancelCurrentDownload() {
        prewarmTimer.stop()
        downloadController.cancelDownload()
        showToast("已取消")
    }

    function requestCancel() {
        if (downloadController.downloadedBytes <= 0) {
            cancelCurrentDownload()
            return
        }
        pendingDiscardAction = "cancel"
        pendingDiscardUrl = ""
        confirmDialog.open()
    }

    function confirmDiscard() {
        const action = pendingDiscardAction
        const nextUrl = pendingDiscardUrl
        pendingDiscardAction = ""
        pendingDiscardUrl = ""
        confirmDialog.close()
        if (action === "replace") {
            prewarmTimer.stop()
            downloadController.startDownload(nextUrl)
        } else if (action === "restart") {
            downloadController.restartDownload()
        } else {
            cancelCurrentDownload()
        }
    }

    function handleDroppedUrl(drop) {
        let value = ""
        if (drop.urls && drop.urls.length > 0)
            value = drop.urls[0].toString()
        if ((value.length === 0 || value.startsWith("file:")) && drop.text)
            value = drop.text.trim()
        if (value.startsWith("http://") || value.startsWith("https://")) {
            urlField.text = value
            prewarmTimer.restart()
            showToast("链接已载入")
        }
    }

    function primaryAction() {
        if (!downloadController.hasTask || hasNewUrl()) {
            startFromField()
            return
        }
        switch (downloadController.state) {
        case "Downloading":
        case "Probing":
            downloadController.pauseDownload()
            break
        case "Paused":
            downloadController.resumeDownload()
            break
        case "Selecting":
            hlsDialog.open()
            break
        case "Failed":
            if (downloadController.sourceChanged) {
                pendingDiscardAction = "restart"
                pendingDiscardUrl = downloadController.url
                confirmDialog.open()
            } else {
                downloadController.retry()
            }
            break
        case "Completing":
            break
        case "Completed":
            if (!downloadController.openDownloadFolder())
                showToast("无法打开下载目录")
            break
        default:
            startFromField()
        }
    }

    function startFromField() {
        const text = urlField.text.trim()
        prewarmTimer.stop()
        if (text.length === 0) {
            showToast("请先粘贴一个下载直链")
            return
        }
        if (downloadController.hasTask
                && downloadController.downloadedBytes > 0
                && downloadController.state !== "Completed"
                && hasNewUrl()) {
            pendingDiscardAction = "replace"
            pendingDiscardUrl = text
            confirmDialog.open()
            return
        }
        downloadController.startDownload(text)
        if (downloadController.hasTask)
            urlField.text = downloadController.url
    }

    function showToast(message) {
        toastText = message
        toastVisible = true
        toastTimer.restart()
    }

    Component.onCompleted: {
        if (downloadController.hasTask)
            urlField.text = downloadController.url
        else if (initialDisplayUrl.length > 0)
            urlField.text = initialDisplayUrl
        else
            Qt.callLater(function() { pasteClipboard(false) })
        if (autoStart)
            Qt.callLater(function() { downloadController.startDownload(initialUrl) })
        else if (downloadController.restoredTask)
            showToast("已恢复未完成任务")
    }

    onActiveChanged: {
        if (active && !downloadController.hasTask
                && urlField.text.trim().length === 0)
            Qt.callLater(function() { pasteClipboard(false) })
    }

    Connections {
        target: downloadController

        function onStateChanged() {
            if (downloadController.state === "Completed")
                showToast("下载完成")
            if (downloadController.state === "Failed"
                    && !downloadController.hasTask
                    && downloadController.errorString.length > 0)
                showToast(downloadController.errorString)
            if (downloadController.state === "Selecting")
                Qt.callLater(function() { hlsDialog.open() })
            else if (hlsDialog.visible)
                hlsDialog.close()
        }

    }

    FolderDialog {
        id: folderDialog
        title: "选择下载目录"
        onAccepted: downloadController.setDownloadDirectory(selectedFolder)
    }

    SettingsDialog {
        id: settingsDialog
        themeRef: theme
        directoryText: downloadController.downloadDirectory
        proxyLabel: proxyModeLabel()
        proxyAccent: proxyModeColor()
        routeText: downloadController.activeRoute
        routeNoteText: downloadController.routeNote
        connectionLimitText: downloadController.connectionLimit + " 路"
        directoryEditable: !downloadController.hasTask
            || downloadController.state === "Completed"
        connectionEditable: !downloadController.hasTask
            || downloadController.state === "Completed"
        settingsEditable: downloadController.state !== "Downloading"
            && downloadController.state !== "Probing"
            && downloadController.state !== "Paused"
            && downloadController.state !== "Completing"
            && downloadController.state !== "Selecting"
        onChangeDirectoryRequested: {
            settingsDialog.close()
            folderDialog.open()
        }
        onOpenDirectoryRequested: {
            if (!downloadController.openDownloadFolder())
                showToast("无法打开下载目录")
        }
        onCycleProxyRequested: cycleProxyMode()
        onCycleConnectionLimitRequested: {
            downloadController.cycleConnectionLimit()
            showToast("下载通道：" + downloadController.connectionLimit)
        }
        onManageProxyRequested: {
            settingsDialog.close()
            builtinDialog.open()
        }
    }

    Shortcut {
        sequences: [StandardKey.Preferences]
        onActivated: settingsDialog.open()
    }

    Shortcut {
        sequences: [StandardKey.New]
        onActivated: {
            urlField.forceActiveFocus()
            urlField.selectAll()
        }
    }

    Dialog {
        id: hlsDialog
        modal: true
        width: 440
        height: 290
        anchors.centerIn: Overlay.overlay
        padding: 0
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            radius: 24
            color: theme.card
            border.width: 1
            border.color: theme.border
        }

        contentItem: ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 8

            Text {
                text: "选择清晰度"
                color: theme.textPrimary
                font.family: theme.uiFont
                font.pixelSize: 17
                font.weight: Font.Bold
            }

            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: 190
                model: downloadController.hlsVariants
                spacing: 6
                clip: true

                delegate: AppButton {
                    required property int index
                    required property string modelData
                    width: ListView.view.width
                    height: 36
                    text: modelData
                    primary: index === 0
                    accent: theme.cyan
                    panelColor: theme.elevated
                    borderColor: theme.border
                    uiFont: theme.uiFont
                    numericFont: theme.numericFont
                    onClicked: {
                        hlsDialog.close()
                        downloadController.selectHlsVariant(index)
                    }
                }
            }

            AppButton {
                text: "取消"
                Layout.alignment: Qt.AlignRight
                Layout.preferredWidth: 72
                Layout.preferredHeight: 30
                accent: theme.danger
                panelColor: "#FFF4F5"
                borderColor: theme.danger
                textColor: theme.danger
                uiFont: theme.uiFont
                numericFont: theme.numericFont
                onClicked: hlsDialog.close()
            }
        }
    }

    Dialog {
        id: confirmDialog
        modal: true
        width: 320
        height: 154
        anchors.centerIn: Overlay.overlay
        padding: 0
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            radius: 24
            color: theme.card
            border.width: 1
            border.color: theme.border
        }

        contentItem: ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 8

            Text {
                text: root.pendingDiscardAction === "restart"
                    ? "重新下载？"
                    : (root.pendingDiscardAction === "replace"
                        ? "开始新任务？" : "取消下载？")
                color: theme.textPrimary
                font.family: theme.uiFont
                font.pixelSize: 16
                font.weight: Font.Bold
            }

            Text {
                Layout.fillWidth: true
                text: root.pendingDiscardAction === "restart"
                    ? "未完成文件将被删除并重新开始"
                    : (root.pendingDiscardAction === "replace"
                        ? "当前未完成文件将被删除" : "未完成文件将被删除")
                color: theme.textMuted
                font.family: theme.uiFont
                font.pixelSize: 10
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Item { Layout.fillWidth: true }

                AppButton {
                    text: "返回"
                    Layout.preferredWidth: 82
                    Layout.preferredHeight: 32
                    accent: theme.cyan
                    panelColor: theme.elevated
                    borderColor: theme.border
                    uiFont: theme.uiFont
                    numericFont: theme.numericFont
                    onClicked: confirmDialog.close()
                }

                AppButton {
                    text: root.pendingDiscardAction === "restart"
                        ? "重下"
                        : (root.pendingDiscardAction === "replace"
                            ? "替换" : "取消")
                    Layout.preferredWidth: 68
                    Layout.preferredHeight: 32
                    accent: theme.danger
                    panelColor: "#FFF4F5"
                    borderColor: theme.danger
                    textColor: theme.danger
                    uiFont: theme.uiFont
                    numericFont: theme.numericFont
                    onClicked: confirmDiscard()
                }
            }
        }

        onClosed: {
            root.pendingDiscardAction = ""
            root.pendingDiscardUrl = ""
        }
    }

    Dialog {
        id: builtinDialog
        property bool clearPending: false
        modal: true
        width: 500
        height: 204
        anchors.centerIn: Overlay.overlay
        padding: 0
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            radius: 26
            color: theme.card
            border.width: 1
            border.color: theme.border
        }

        contentItem: ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                Text {
                    text: "内置 Clash"
                    color: theme.textPrimary
                    font.family: theme.uiFont
                    font.pixelSize: 16
                    font.weight: Font.Bold
                    font.letterSpacing: 1.0
                }
                Item { Layout.fillWidth: true }
                ReleasePill {
                    text: downloadController.embeddedProxyRunning
                        ? "运行中" : "未运行"
                    accent: downloadController.embeddedProxyRunning
                        ? theme.success : theme.cyan
                    themeRef: theme
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 40
                radius: 14
                color: "#F6FAFF"
                border.width: 1
                border.color: theme.border

                TextField {
                    id: builtinSubscriptionField
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    color: theme.textPrimary
                    placeholderText: "订阅链接"
                    placeholderTextColor: theme.textMuted
                    selectionColor: theme.cyan
                    selectedTextColor: "#FFFFFF"
                    echoMode: TextInput.Password
                    font.family: theme.uiFont
                    font.pixelSize: 12
                    selectByMouse: true
                    background: Item {}
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                AppButton {
                    text: "导入并启用"
                    primary: true
                    Layout.fillWidth: true
                    Layout.preferredHeight: 34
                    accent: theme.cyan
                    panelColor: theme.elevated
                    borderColor: theme.border
                    uiFont: theme.uiFont
                    numericFont: theme.numericFont
                    enabled: builtinSubscriptionField.text.trim().length > 8
                    onClicked: {
                        downloadController.importEmbeddedSubscription(
                            builtinSubscriptionField.text.trim())
                        builtinSubscriptionField.text = ""
                        clearPending = false
                    }
                }

                AppButton {
                    text: builtinDialog.clearPending ? "确认清除" : "清除"
                    visible: downloadController.embeddedProxyConfigured
                    Layout.preferredWidth: builtinDialog.clearPending ? 82 : 58
                    Layout.preferredHeight: 34
                    accent: theme.danger
                    panelColor: "#FFF4F5"
                    borderColor: theme.danger
                    textColor: theme.danger
                    uiFont: theme.uiFont
                    numericFont: theme.numericFont
                    onClicked: {
                        if (builtinDialog.clearPending) {
                            downloadController.clearEmbeddedSubscription()
                            builtinDialog.clearPending = false
                        } else {
                            builtinDialog.clearPending = true
                        }
                    }
                }

                AppButton {
                    text: "关闭"
                    Layout.preferredWidth: 96
                    Layout.preferredHeight: 34
                    accent: theme.cyan
                    panelColor: theme.elevated
                    borderColor: theme.border
                    uiFont: theme.uiFont
                    numericFont: theme.numericFont
                    onClicked: builtinDialog.close()
                }
            }

            Text {
                Layout.fillWidth: true
                text: downloadController.embeddedProxyStatus
                color: downloadController.embeddedProxyConfigured
                    ? theme.success : theme.textMuted
                font.family: theme.uiFont
                font.pixelSize: 9
                elide: Text.ElideRight
            }

        }

        onClosed: clearPending = false
    }

    Timer {
        id: toastTimer
        interval: 2600
        repeat: false
        onTriggered: toastVisible = false
    }

    Timer {
        id: prewarmTimer
        interval: 650
        repeat: false
        onTriggered: downloadController.prewarmProxyForUrl(urlField.text)
    }

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#EEF6FF" }
            GradientStop { position: 0.56; color: "#FFFFFF" }
            GradientStop { position: 1.0; color: "#E8F2FF" }
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 2
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop { position: 0.18; color: theme.cyan }
            GradientStop { position: 0.82; color: theme.pink }
            GradientStop { position: 1.0; color: "transparent" }
        }
        opacity: 0.45
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            spacing: 9

            Image {
                Layout.preferredWidth: 30
                Layout.preferredHeight: 30
                source: "qrc:/branding/kuaixiazai-mark.svg"
                sourceSize.width: 60
                sourceSize.height: 60
                fillMode: Image.PreserveAspectFit
                smooth: true
            }

            Text {
                Layout.alignment: Qt.AlignVCenter
                text: "快下载"
                color: theme.textPrimary
                font.family: theme.uiFont
                font.pixelSize: 18
                font.weight: Font.Bold
                font.letterSpacing: 1.0
            }

            Item { Layout.fillWidth: true }

            AppButton {
                text: "设置"
                Layout.preferredWidth: 54
                Layout.preferredHeight: 28
                accent: theme.cyan
                panelColor: theme.elevated
                borderColor: theme.border
                uiFont: theme.uiFont
                numericFont: theme.numericFont
                onClicked: settingsDialog.open()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            radius: 14
            color: theme.card
            border.width: 1
            border.color: theme.border

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 8
                spacing: 8

                TextField {
                    id: urlField
                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
                    enabled: downloadController.state !== "Downloading"
                        && downloadController.state !== "Probing"
                        && downloadController.state !== "Completing"
                        && downloadController.state !== "Selecting"
            && downloadController.state !== "Selecting"
                    color: theme.textPrimary
                    placeholderText: "粘贴下载链接"
                    placeholderTextColor: theme.textMuted
                    selectionColor: theme.cyan
                    selectedTextColor: "#FFFFFF"
                    font.family: theme.uiFont
                    font.pixelSize: 12
                    selectByMouse: true
                    background: Item {}
                    onTextChanged: {
                        if (!autoStart && text.trim().length > 0)
                            prewarmTimer.restart()
                        else
                            prewarmTimer.stop()
                    }
                    onAccepted: primaryAction()
                }

                AppButton {
                    text: "粘贴"
                    visible: !root.transferActive()
                    Layout.preferredWidth: 54
                    Layout.preferredHeight: 36
                    accent: theme.cyan
                    panelColor: theme.elevated
                    borderColor: theme.border
                    textColor: theme.cyan
                    uiFont: theme.uiFont
                    numericFont: theme.numericFont
                    onClicked: pasteClipboard(true)
                }

                AppButton {
                    text: "取消"
                    visible: root.transferActive()
                    Layout.preferredWidth: 62
                    Layout.preferredHeight: 36
                    accent: theme.danger
                    panelColor: "#FFF4F5"
                    borderColor: theme.danger
                    textColor: theme.danger
                    uiFont: theme.uiFont
                    numericFont: theme.numericFont
                    onClicked: requestCancel()
                }

                AppButton {
                    text: primaryActionText()
                    primary: true
                    Layout.preferredWidth: 80
                    Layout.preferredHeight: 36
                    accent: theme.cyan
                    panelColor: theme.elevated
                    borderColor: theme.border
                    uiFont: theme.uiFont
                    numericFont: theme.numericFont
                    enabled: downloadController.hasTask
                        || urlField.text.trim().length > 0
                    onClicked: primaryAction()
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: downloadController.hasTask
            radius: 16
            color: theme.card
            border.width: 1
            border.color: theme.border
            clip: true

            gradient: Gradient {
                orientation: Gradient.Vertical
                GradientStop { position: 0.0; color: "#FFFFFF" }
                GradientStop { position: 1.0; color: "#F3F8FF" }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 7

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Text {
                        Layout.fillWidth: true
                        text: downloadController.fileName
                        color: theme.textPrimary
                        font.family: theme.uiFont
                        font.pixelSize: 16
                        font.weight: Font.Bold
                        elide: Text.ElideRight
                    }

                    Text {
                        text: downloadController.protocolName
                        color: theme.textMuted
                        font.family: theme.numericFont
                        font.pixelSize: 8
                        font.letterSpacing: 0.5
                    }

                    Text {
                        text: root.progressValue() > 0
                            || downloadController.state === "Completed"
                            ? (root.progressValue() * 100).toFixed(1) + "%"
                            : "--"
                        color: theme.cyan
                        font.family: theme.numericFont
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                    }
                }

                Rectangle {
                    id: progressTrack
                    Layout.fillWidth: true
                    Layout.preferredHeight: 8
                    radius: 4
                    color: "#DCE8F8"

                    Rectangle {
                        width: root.progressValue() > 0
                            || downloadController.state === "Completed"
                            ? progressTrack.width * root.progressValue()
                            : 0
                        height: parent.height
                        radius: 4
                        color: theme.cyan
                        Behavior on width {
                            NumberAnimation {
                                duration: 160
                                easing.type: Easing.OutCubic
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 34
                    spacing: 7

                    Text {
                        text: downloadController.state === "Downloading"
                            ? downloadController.speedMBps.toFixed(1)
                            : root.taskStatusText()
                        color: downloadController.state === "Failed"
                            ? theme.danger : theme.textPrimary
                        font.family: theme.numericFont
                        font.pixelSize: downloadController.state === "Downloading"
                            ? 24 : 16
                        font.weight: Font.Bold
                    }

                    Text {
                        text: "MB/s"
                        visible: downloadController.state === "Downloading"
                        color: theme.cyan
                        font.family: theme.numericFont
                        font.pixelSize: 9
                        Layout.alignment: Qt.AlignBottom
                        Layout.bottomMargin: 5
                    }

                    Item { Layout.fillWidth: true }

                    Text {
                        text: downloadController.protocolName === "HLS"
                            ? formatBytes(downloadController.downloadedBytes)
                                + (downloadController.activeLanes > 0
                                    ? " · " + downloadController.activeLanes + " 路" : "")
                            : formatBytes(downloadController.downloadedBytes)
                                + " / " + (downloadController.totalBytes > 0
                                    ? formatBytes(downloadController.totalBytes)
                                    : "未知大小")
                                + (downloadController.state === "Downloading"
                                    && downloadController.activeLanes > 0
                                    ? " · " + downloadController.activeLanes + " 路" : "")
                        color: theme.textSecondary
                        font.family: theme.numericFont
                        font.pixelSize: 9
                    }

                    Rectangle {
                        Layout.preferredWidth: 1
                        Layout.preferredHeight: 14
                        color: theme.border
                    }

                    Text {
                        id: transferTrailingText
                        Layout.maximumWidth: downloadController.state === "Failed"
                            ? 250 : 120
                        text: downloadController.state === "Completed"
                                && downloadController.sha256.length > 0
                            ? "SHA-256 ✓"
                            : (downloadController.state === "Failed"
                                ? downloadController.errorString
                                : etaText())
                        color: downloadController.state === "Failed"
                            ? theme.danger
                            : (downloadController.state === "Completed"
                                ? theme.success : theme.textMuted)
                        font.family: theme.uiFont
                        font.pixelSize: 9
                        elide: Text.ElideRight

                        MouseArea {
                            id: trailingHover
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: downloadController.state === "Completed"
                                && downloadController.sha256.length > 0
                                ? Qt.PointingHandCursor : Qt.ArrowCursor
                            onClicked: {
                                if (downloadController.state === "Completed"
                                        && downloadController.copySha256())
                                    root.showToast("SHA-256 已复制")
                            }
                        }

                        ToolTip.visible: trailingHover.containsMouse
                            && ((downloadController.state === "Completed"
                                    && downloadController.sha256.length > 0)
                                || (downloadController.state === "Failed"
                                    && downloadController.errorString.length > 0))
                        ToolTip.text: downloadController.state === "Completed"
                            ? downloadController.sha256
                            : downloadController.errorString
                        ToolTip.delay: 300
                    }
                }
            }
        }
    }

    DropArea {
        anchors.fill: parent
        onDropped: root.handleDroppedUrl(drop)
    }

    Rectangle {
        id: toast
        z: 100
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 34
        width: Math.min(parent.width - 80, toastLabel.implicitWidth + 44)
        height: 42
        radius: 21
        color: theme.elevated
        border.width: 1
        border.color: theme.cyan
        opacity: root.toastVisible ? 1 : 0
        visible: opacity > 0

        Behavior on opacity {
            NumberAnimation { duration: 160 }
        }

        Text {
            id: toastLabel
            anchors.centerIn: parent
            text: root.toastText
            color: theme.textPrimary
            font.family: theme.uiFont
            font.pixelSize: 12
        }
    }

    component ReleasePill: Rectangle {
        property string text: ""
        property color accent: "#2F73E8"
        property var themeRef: null
        implicitWidth: 50
        implicitHeight: 22
        radius: 11
        color: "#EAF2FC"
        border.width: 1
        border.color: accent
        Text {
            anchors.centerIn: parent
            text: parent.text
            color: parent.accent
            font.family: parent.themeRef ? parent.themeRef.uiFont : "Avenir Next"
            font.pixelSize: 8
            font.letterSpacing: 1.0
        }
    }
}
