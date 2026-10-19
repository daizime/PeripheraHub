# 四格玻璃应用图标

用户于 2026-10-19 选定概念版图的第二种。源图由内置 imagegen 生成，按选定图形提取为透明背景；不是微软官方应用标识。界面内的 Fluent System Icons 仍按原 MIT 许可维护。

- 母版：`four-glass-master.png`，保留生成结果及 alpha。
- 运行时 PNG 和 Windows ICO：`src/app/widgets/icons/app/`，逐项 SHA256 见同目录 `manifest.json`。
- Qt 使用 16/20/24/32/40/48/64/128/256/512px PNG；ICO 含 16–256px 共 9 档 PNG 帧，嵌入 EXE 的图标资源 1。
- 开发工具 `tools/app-icon-builder` 只缩放、导出和封装格式；没有重新生成或修改图形内容。运行时不依赖此工具或额外 ICO 图片插件。

使用方式：在 MSVC x64 环境下，用 Qt Gui 构建 `tools/app-icon-builder`，从项目根目录执行：

```
app_icon_builder assets/branding/four-glass-master.png src/app/widgets/icons/app work/verification/app-brand-icon/icon-size-preview.png
app_icon_builder --verify-exe <构建后的 PeripheralHub.exe>
```

生成方式：内置 image_gen（透明背景编辑），未使用 API CLI fallback。参考图是本会话四方案版图，生成结果默认文件名为 `exec-d3022e3f-9bfc-45a5-8c5e-5eac69eeec77.png`，已复制至本目录母版。

最终提示词：

```
Use case: background-extraction. Edit target: the supplied concept board. Extract ONLY the large application icon from quadrant 02, titled 四格玻璃模块 (top right). Preserve this specific approved icon's appearance, rounded shapes, cyan-blue/periwinkle-violet palette, glossy frosted glass highlights, slight asymmetric 2x2 layout: large cyan top-left module, smaller violet top-right module, smaller violet bottom-left module, large blue-cyan bottom-right module. Keep the precise relative proportions and balanced spacing; do not redesign it and do not replace it with a Windows logo. Remove ALL background, titles, text, recommendation labels, other icons, and small sample tiles. Output one standalone square high-resolution application icon on a genuinely transparent alpha background. Center the four-module emblem so its outer silhouette occupies approximately 90% of the square canvas with even transparent safety margins. Preserve opaque vibrant color inside the glass modules and crisp antialiased outer edges; no broad drop shadow or glow in the transparent exterior. Front facing, same material and shape as the original approved quadrant 02. No text, no watermark, no backplate. This is for a desktop application's executable, taskbar and tray icon.
```
