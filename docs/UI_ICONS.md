# Fluent UI 图标

采用微软官方 [Fluent System Icons](https://github.com/microsoft/fluentui-system-icons)，固定提交与逐项 SHA-256 见 `third_party/fluent-system-icons/manifest.json`，MIT 原文见同目录 `LICENSE`。

## 使用规则

- 导航默认 regular，选中状态 filled；浅色玻璃侧栏统一使用蓝色图标。
- 四类卡片使用 Cursor、Keyboard、Desktop、Headphones；当前固定版本用指针表达鼠标类别。
- 亮度、刷新、设备详情、连接关联、更多/收起、下拉箭头、信息提示及托盘菜单使用同一图标集。
- 输出和输入静音分别使用扬声器、麦克风图标；实际读回后切换静音图形。目标无效时复位图形、操作文字和无障碍名称，并禁用操作。
- 所有操作保留文字；图标不替代电量、在线状态或功能能力的检测结果。

## 资源与扩展入口

`UiTheme.*` 提供 `UiIcon` 与 `fluentIcon()`，统一着色、禁用态、regular/filled 和缓存。`DeviceCard::setCategoryIcon()` 设置卡片类别图形。图标 PNG 经 `UiTheme.qrc` 嵌入程序，正常构建与运行均无需联网。

官方 SVG 保留在 `third_party/fluent-system-icons/svg`。开发工具 `tools/fluent-icon-renderer` 将 24px SVG 渲染成 96×96 透明 PNG，工具使用本机 Qt Svg；应用和打包仍只需要 Qt Core、Gui、Widgets。导入和重新渲染命令见该工具的 README。

## 资源核验与运行目录

现有 40 个官方 SVG（20 个图形的 regular / filled）保留固定提交与逐项 SHA-256；2026-10-19 补充 Info 用于提示栏、Settings 用于程序设置。新增图形沿用 manifest 的微软仓库固定提交与 MIT 许可。图标导入脚本与枚举、资源清单同步维护。

初版图标验证产物在 `work/verification/fluent-icons`；整窗参考布局的后续验证在 `work/verification/reference-ui`。`*-fixture.png` 为模拟数据，截图与界面检查不能证明新增设备协议适配。

正式启动入口为 `outputs/PeripheralHub-0.0/PeripheralHub.exe`。保留包内 DLL、插件和 data；旧的 `PeripheralHub-Fluent` 目录已不作为交付入口。

## 应用品牌图标（2026-10-19）

按用户选定的第二种“四格玻璃模块”方案，统一 EXE 文件、窗口、任务栏、托盘及侧栏品牌图形。该品牌图像由内置 imagegen 从已确认版图提取，母版和最终提示词见 `assets/branding/README.md`，不属于微软官方 Fluent 图标。导航和功能图形继续使用 Fluent 图标集。

`applicationIcon()` 从资源中加载 16/20/24/32/40/48/64/128/256/512px PNG，不需要额外图片插件。Windows EXE 另嵌入 16–256px 的 9 档透明 ICO 帧，资源 ID 为 1；CMake 在构建目录复制 ASCII 文件名，避免资源编译器处理含中文的绝对图片路径。`tools/app-icon-builder` 负责尺寸导出、ICO 封装与 Windows 资源加载核验，运行时不依赖开发工具。

格式来源：[微软：ICON 资源](https://learn.microsoft.com/en-us/windows/win32/menurc/icon-resource)、[微软：PNG 压缩图标格式](https://devblogs.microsoft.com/oldnewthing/20101022-00/?p=12473)、[微软：主图标资源 ID](https://devblogs.microsoft.com/oldnewthing/20250423-00/?p=111106)。

验证目录为 `work/verification/app-brand-icon`。小尺寸预览使用实际导出的 PNG，在浅、深两种背景中按 1:1 像素显示。Windows 固定任务栏或旧快捷方式可能保留先前图标缓存，重启程序后若仍显示旧图标，可重新固定快捷方式；应用不清理系统全局图标缓存。
