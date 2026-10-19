# 第三方组件与研究来源

本文件记录初版依赖和协议研究来源；不是整个依赖树审计完成的声明。以实际构建及打包清单为准。

项目原创程序代码的许可为 AGPL-3.0-or-later，许可原文见 `LICENSE`、授予说明见 `COPYING.md`。以下第三方材料继续适用各自许可。

## Qt 6

- 官方项目：https://www.qt.io/
- 本机开发版本：Qt 6.7.0，`msvc2019_64` 安装目录。
- 直接使用：Qt Core、Gui、Widgets；当前核心测试通过 CTest 运行，不依赖 Qt Test。
- 开源许可：使用前须核定对应模块及版本的 LGPLv3 / GPLv3 选项与第三方组件通知。
- 本机许可原文来源：`F:\qt\6.7.0\Src\LICENSES`、`qtbase/LICENSES` 以及实际部署模块的许可目录。
- 打包脚本复制上述许可文本。完整对应源码的交付安排见 `docs/DISTRIBUTION.md`。

当前精简部署还包含 `platforms/qwindows.dll` 和 `styles/qmodernwindowsstyle.dll`；插件路径由包内 `qt.conf` 指定。打包按白名单复制，不自动加入 Network、TLS、SVG、OpenGL 软件回退或 DXC 组件。应以输出包的 `dependency-manifest.json` 为起点核对实际文件及许可，不能仅按三个直接链接模块判断，也不能把最小文件集合视为许可审计已经完成。

## Windows API 与 Microsoft 运行库

Windows 系统 API 包括 HID、SetupAPI、Core Audio 和 Dxva2；系统 DLL 不从系统目录手动复制进包。MSVC 运行库采用应用本地 DLL，从 Visual Studio 的 x64 `Microsoft.VC*.CRT` 可再分发目录复制，可通过 `-RuntimeDirectory` 指定；当前不附运行库安装器。实际再分发条件仍以安装的 Microsoft 开发工具条款为准。

毛玻璃主题另外通过 Windows 系统 `dwmapi.dll` 请求 Desktop Acrylic，不引入或复制额外第三方运行库。使用公开 DWM 接口，官方文档与系统要求见 `docs/UI_GLASS.md`。

## Fluent System Icons

- 官方仓库：[microsoft/fluentui-system-icons](https://github.com/microsoft/fluentui-system-icons)。
- 本次固定来源提交：`f1a33bcbe1b133cf36bbb80b2dc7ca935a1011bf`，不声称为最新版本。
- 版权：Copyright (c) 2020 Microsoft Corporation；MIT 许可原文保存在 `third_party/fluent-system-icons/LICENSE`。
- 仅引入 `manifest.json` 中列出的官方 regular / filled SVG；每项记录上游路径与 SHA-256，SVG 保留原文。鼠标类别采用官方 Cursor 指针图标。
- 开发工具 `tools/fluent-icon-renderer` 使用本机 Qt Svg 将图标预渲染为 96×96 透明 PNG；运行时由 Qt Gui 着色、缩放并嵌入资源，不增加 Qt Svg 运行依赖。默认线框，侧栏选中使用实心；应用品牌标识继续使用原有绘制。
- 打包脚本将该 MIT 许可和来源清单复制到 `licenses/FluentSystemIcons`。

## 设备协议研究来源

- [BatteryMonitor](https://github.com/JKWTCN/BatteryMonitor)：MIT。AULA/VGN 电量协议研究线索；仅引用项目并不意味着整套代码或其依赖已纳入本程序。
- [OpenMouse mouse-protocol](https://github.com/OpenMouse-Project/mouse-protocol)：固定提交 `54c68c69bbd0bcd750cf10a4e439148e634c02db`，`package.json` 声明 AGPL-3.0-or-later。VGN 报文和行为的研究线索；当前尚未移植其完整配置实现。其默认范围、推定按键表不能当成本机型号已验证功能。
- [Monitorian](https://github.com/emoacht/Monitorian)：MIT。Windows DDC/CI 的研究参考。
- [langtucontroller](https://github.com/justsomeon123/langtucontroller)：固定提交 `ddf9ab1114bb876507ec5bafe99d705763d1f555` 未发现 LICENSE，Cargo 清单未声明许可；不复制或移植其 RGB 代码。用户授权项目开源不能替代该上游作者授权。
- [atk-n9-mac](https://github.com/ibroxiim/atk-n9-mac)：固定提交 `d95a2d36f673456d7cada9aa2db102d2b01064e6`，项目提供 MIT / Apache-2.0 双许可。研究的是旧 N9 串口通道，其已测 Ultra 接收器 PID `1204` 与本机 V2 的 `12D6` 不同；未将旧命令发送到本机厂商 HID 接口。

2026-10-19 另查阅 [VGN 官方网页驱动](https://drive.vgnlab.com.cn/) 的公开查询流程，仅用于互操作研究与只读诊断。厂家 JavaScript 不属于本项目开源代码，不复制或收入产品及源码包。研究下载、实机标识和原始报文只保留在 `work/`，不打入分发材料。

F2 Master+ 的配置字段根据本机回复、厂商公开格式与上述协议研究独立实现；具体资源哈希、上游高 DPI 解码限制、可读字段和未验收项目见 `docs/VGN_F2_PROTOCOL.md`。自动化测试保留剔除个人身份的实机配置报文样本，完整研究下载与个人接口路径不进入源码归档。

N9 V2 的查询格式另根据 [ATK 官方网页驱动](https://hub.atk.pro/) 公开资源及本机实际响应独立实现，来源固定资源、哈希与协议事实见 `docs/N9_V2_PROTOCOL.md`。厂商 JavaScript 未复制、链接或收入源码归档；没有将公开访问等同于开源许可。

当前初版没有引入 hidapi、NAudio、HidSharp、spdlog、Equalizer APO、OpenRGB 或 HeadsetControl 二进制。后续如果实际引入，应更新本文件并保留适用的版权、许可和 NOTICE。
