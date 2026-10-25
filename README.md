# QuickDL V2.0 Beta

当前目录包含可发布的 Qt 6 桌面 Beta。主界面使用 Qt Quick/QML，
传输核心使用 C++、libcurl multi 与 Qt Network。

当前版本为 **0.3.0**，已覆盖 V2.0 的单任务 HTTP、HLS VOD 与 FTP/FTPS：
小分块工作窃取、429 退避、慢尾拆分、SQLite 重启恢复、源文件变化保护、
AES-128 和 SHA-256 完成校验。阶段记录见
[V2 迭代记录](docs/V2-迭代记录-2026-10-21.md)。

如需在新对话中继续开发，先阅读
[Beta 发布说明](docs/BETA-发布说明-2026-10-24.md) 和
[续接摘要](docs/续接摘要-2026-10-20.md)，不要重新携带整段历史聊天和截图。

## 运行

已构建的 macOS 应用：

```text
build/QuickDL.app
```

直接双击即可打开。支持 HTTP、HTTPS、HLS VOD `.m3u8`、FTP 和 FTPS。

从源码构建：

```bash
/Users/gzhysu/Qt/Tools/CMake/CMake.app/Contents/bin/cmake \
  -S . -B build -G Ninja \
  -DCMAKE_PREFIX_PATH=/Users/gzhysu/Qt/6.11.1/macos \
  -DCMAKE_BUILD_TYPE=Debug

/Users/gzhysu/Qt/Tools/CMake/CMake.app/Contents/bin/cmake --build build
```

## 当前已实现

- 原生 Qt Quick 桌面窗口
- 600 × 118 空闲工具条；有任务时展开到 600 × 238，设置集中在独立弹窗
- 中文界面；仅保留 Qt、HTTP、Range、libcurl、Clash、mihomo 等必要技术名词
- 统一 Protocol Router：HTTP、HTTPS、HLS、FTP、FTPS
- HTTP/HTTPS 直链下载
- HEAD 文件探测与 Range 能力判断
- 默认 16 路、最高 32 路真实 Range 并行下载
- 4～32 MiB 小分块工作窃取调度，完成一块后立即领取下一块
- 4 路起步的渐进连接爬升
- 429/5xx 自动退避，遵守最长 60 秒的 `Retry-After`
- 无排队分片时对停滞 4 秒的慢尾进行实时二分
- Range 不可用时降级单连接
- 单个分段遇到临时 TLS、连接中断或超时会自动续传重试两次
- 分段下载失败时保留已完成偏移，只重新请求未完成区间
- 真实下载字节写盘
- 真实速度、剩余时间与活跃连接数
- 暂停、继续，以及失败后从现有分段偏移重试
- 失败原因直接显示在任务卡中
- 取消或替换未完成任务前确认，避免误删 `.part`
- 完成前检查临时文件总大小，校验失败时保留未完成文件
- 下载完成后异步计算 SHA-256，点击 `SHA-256 ✓` 可复制
- SQLite 持久化任务、分段偏移、ETag 和 Last-Modified
- 进程被强制结束或正常退出后，下次启动恢复为暂停任务
- ETag、Last-Modified、最终地址或文件大小变化时禁止拼接
- HLS VOD master/media playlist 解析与清晰度选择
- HLS 8 路分片并发、重试、暂停和启动恢复
- HLS AES-128 标准密钥获取与解密
- HLS `.ts`、fMP4 `.m4s`、AAC 与 WebVTT 分片顺序合并
- FTP/FTPS 用户名密码下载、被动模式、SIZE 与 REST 续传
- FTPS 显式 TLS，默认校验证书，拒绝自签名证书静默降级
- macOS Keychain 与 Windows Credential Manager 保存 FTP 密码
- 智能代理路由：自动、内置 mihomo、iKuuu 强制代理、强制直连
- 内置 mihomo v1.19.32 子进程，应用内导入 Clash/Mihomo 订阅
- 订阅链接保存在 macOS 系统钥匙串，生成配置权限为 `0600`
- 内置代理仅监听本机 `18890/18891`，不修改系统代理
- 自动识别海外资源并切换本机 iKuuu HTTP/SOCKS 代理
- iKuuu 未运行时可自动启动，并通过 `ikuuu:///vpn/toggle` 发起连接
- 等待代理隧道真正可访问 HTTPS 后再开始下载，最长等待 45 秒
- 粘贴海外链接后提前预热 iKuuu，把启动与用户输入操作重叠
- 任务结束后保留 120 秒热连接，期间新任务直接复用，避免再次冷启动
- 只关闭由 QuickDL 自动启动的 iKuuu；用户原本已运行的实例保持不变
- 直连与代理之间的探测失败、下载失败自动回退
- 保存目录选择
- 设置中可随时打开下载目录，完成后主按钮也可直接打开
- 内置代理状态反馈与二次确认清除
- `Cmd/Ctrl + N` 聚焦下载链接输入框
- 清爽的蓝白配色，深海军蓝文字、主蓝操作按钮、浅蓝分层表面与同色系应用图标

## Beta 限制

- 当前仍是单任务客户端，不包含多任务队列和历史列表。
- HLS 仅支持 VOD，不支持直播；独立音轨、DASH 与 FFmpeg 重封装不在 Beta 范围。
- FTP/FTPS 为单连接基线，不承诺服务器端多连接分块。
- Linux 尚未接入 Secret Service；需要密码的 FTP/FTPS 在 Linux 上会拒绝
  明文保存。
- 公开分发前需要 Developer ID Application 签名、公证和正式版本号。
- 公网速度必须使用相同 URL、网络和连接数与浏览器/aria2c 对比后才可宣传。
- 海外资源识别使用最终 URL 域名规则；未知域名默认按海外资源处理，失败后会回退直连。
- iKuuu 自动启动与自动关闭目前只针对 macOS；其他平台会保持普通代理模式。
- 当前内置 mihomo 二进制为 macOS arm64；其他平台需要补充对应二进制。
- 角色轮播已从主界面移除；未使用的角色资源不会打包进当前 QML 模块。

## 自动化验证

```bash
/Users/gzhysu/Qt/Tools/CMake/CMake.app/Contents/bin/ctest \
  --test-dir build --output-on-failure
```

CTest 覆盖 AES 标准向量、HLS playlist、协议路由、SQLite 恢复，并启动真实
HTTP/HLS 服务器验证 429 重试、慢尾拆分和最终 SHA-256。

## 性能说明

- 截图中的 14～16 MB/s 来自人为限速的测试服务器，不代表 QuickDL 上限。
- 在不限速的同一本地服务器上，6 个独立 curl Range 请求聚合约 137.9 MB/s。
- 16 个独立 Range 参考请求约 376.9 MB/s。
- QuickDL 16 路 libcurl multi 达到约 354.4 MB/s，约为同条件参考上限的 94%。
- 当前仍需使用公网 URL 和不同服务器做浏览器/迅雷同级对比，不能只凭局域网测试宣称全面领先。

## 代理模式

QuickDL 支持内置代理与外部 iKuuu 两种方式。推荐使用内置代理。

点击窗口右上角的代理状态或“设置”进入设置弹窗。代理模式在设置中切换：

- `自动 · 内置`：先直连探测；识别为海外资源时启动内置 mihomo。
- `内置 Clash`：文件探测和下载始终通过内置 mihomo。
- `仅 iKuuu`：文件探测和全部分段始终通过 iKuuu。
- `仅直连`：完全绕过代理和代理环境变量。

在内置代理面板粘贴 Clash/Mihomo 订阅后，QuickDL 会下载并生成本机配置。
内置 mihomo 使用独立端口，代理只在 QuickDL 子进程内生效，不会修改系统代理。
订阅链接不会写入源码、日志或 `QSettings`。

> 内置 mihomo 使用 GPL-3.0。来源、版本和许可证见
> [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

智能模式中，探测或下载连接失败时会自动在直连与代理之间重试一次。

如果未导入内置订阅，QuickDL 仍可使用外部 iKuuu。选择 iKuuu 模式时，
应用会识别系统代理 `http://127.0.0.1:7890`。

如果选择智能或 iKuuu 模式时检测到 `127.0.0.1:7890` 未监听，QuickDL
会启动 `/Applications/iKuuuVPN.app` 并请求连接节点。端口开放后还会通过
`https://cp.cloudflare.com/` 检查代理隧道，避免在 iKuuu 尚处于连接中时
提前开始下载。

任务完成、失败或取消后，QuickDL 会保留本次自动启动的 iKuuu 热连接
120 秒。120 秒内开始新任务会直接复用，不会再次经历冷启动；空闲满
120 秒或 QuickDL 退出时才关闭。如果 iKuuu 在任务开始前已经运行，
QuickDL 不会关闭它。

