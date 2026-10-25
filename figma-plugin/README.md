# QuickDL · 佐助主题 Figma 原型插件

这个插件会在当前 Figma 文件中生成：

- `01 Downloading`：单任务极速下载主界面
- `02 Paused`：暂停状态
- `03 New Download`：新建下载弹窗
- 本地按钮、任务行、指标卡、连接通道组件
- QuickDL 色彩、间距、圆角变量与文字样式
- 可点击原型跳转

## 使用方法

1. 打开 Figma 文件：<https://www.figma.com/design/CSwtpJojOS9oIp0hTxQFCv>
2. 菜单选择 `Plugins → Development → Import plugin from manifest...`
3. 选择本目录中的 `manifest.json`
4. 运行 `QuickDL Sasuke Prototype Builder`
5. 选中生成的 `QuickDL / 01 Downloading` 画板，点击 Present 预览

如果重复运行，插件会先清理以 `QuickDL /` 开头的顶层画板与组件区，再重新生成。

## 更新插件

如果之前已经导入过旧版本，修改 `manifest.json` 后需要重新执行一次：

`Plugins → Development → Import plugin from manifest...`

然后再运行插件。当前版本已为以下能力增加回退：

- Variables 不可用时退回固定色值
- 文字样式不可用时退回手动字体
- Effect Styles 不可用时跳过发光和阴影
- 本地角色图载入失败时保留完整画板结构
- 原型跳转设置失败时不会阻止画板生成

如果仍然失败，插件提示会包含失败阶段，例如
`QuickDL 生成失败【创建文字样式】：...`，把整段文字或截图发回即可定位。

## 素材说明

`code.js` 中嵌入的佐助角色图仅用于本地非商业产品原型，不随公开产品安装包分发。公开发布或商业化前需要确认角色图片的授权范围。
