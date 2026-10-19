# 整窗毛玻璃主题

侧栏、背景、设备卡片、选择框、数值输入、按钮和保存页统一使用浅蓝紫磨砂材质。主题来自 `src/app/widgets/Glass.qss`，弥散光和细颗粒由 `UiTheme.cpp` 的 `GlassSurface` 绘制；设计效果图仅为视觉参考，实际界面为可交互的 Qt Widgets。

## 窗口与背景合成

顶部使用一体式透明标题栏，三个按钮按最小化、最大化/还原、关闭排列。保留 Win32 可缩放窗口，移除系统 caption 样式并处理 `WM_NCCALCSIZE`、边缘及标题栏命中测试；最大化客户区限制到当前显示器工作区。关闭保持原有托盘行为，Ctrl+Q 或托盘“退出”完全退出。

Windows 上先动态探测 `user32.dll` 的 `SetWindowCompositionAttribute`，为整个客户区请求 accent blur，并通过 DWM 扩展玻璃区域。主题只在合成请求成功时降低背景覆盖率，文字与控件仍清晰绘制。本机通过整窗 frame 扩展与 blur/alpha 请求的组合完成合成；接口返回值仍须结合实机观察核验。

兼容风险：微软明确不推荐使用 `SetWindowCompositionAttribute`；accent policy 数字枚举不是稳定 SDK 合同。本项目仅动态加载这条兼容路径，不替换系统 DLL，也不承诺未来 Windows 版本行为一致。调用失败则请求公开的 `DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE, DWMSBT_TRANSIENTWINDOW)`；该枚举要求 Windows 11 Build 22621 或更新版本。仍失败则绘制不透明的同配色磨砂渐变。

Windows 的“透明效果”、节能和系统策略仍可能限制实际模糊。2026-10-18 本机初查 `EnableTransparency=0`；用户明确允许后改为 1。应用自身启动不会修改此全局偏好。接口请求结果不能单独作为实际透明效果的证明。

接口来源：

- [微软：自定义 DWM 窗口边框](https://learn.microsoft.com/en-us/windows/win32/dwm/customframe)
- [微软：SetWindowCompositionAttribute 及兼容性说明](https://learn.microsoft.com/en-us/windows/win32/dwm/setwindowcompositionattribute)
- [微软：DWM_SYSTEMBACKDROP_TYPE](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwm_systembackdrop_type)
- [Qt：QWindow 系统移动与缩放](https://doc.qt.io/qt-6/qwindow.html)
- accent 接口结构和枚举交叉核对：[PyQt-Frameless-Window 上游接口使用](https://github.com/zhiyiYo/PyQt-Frameless-Window/blob/master/qframelesswindow/windows/window_effect.py)。本项目未引入该库或复制其实现。

本机 Qt 的 ARGB/BitBlt 路径另核对 `F:/qt/6.7.0/Src/qtbase/src/plugins/platforms/windows/qwindowsbackingstore.cpp`；不将整个窗口设置为 layered/frameless，不对文字或控件树执行模糊。

## 统一数值输入

`createThemedSpinBox()` 保持返回 QSpinBox。隐藏原生凸起上下箭头，改用同材质的水平减号、加号；支持键盘和完整输入确认，滚轮继续滚动内容。上下限、只读、禁用和忙态同时约束两个按钮。

对比度使用显示器实际回报的原始范围，只有按“应用”才发出设置请求。亮度、音量保留 0–100% 与原有提交规则；刷新读数不会反向提交写入。

## 验证入口

- `work/verification/glass-chrome`：本次独立构建、窗口合成比较、彩色网格背景和回归日志。
- `work/verification/glass-theme`：数值控件交互检查。
- `work/verification/dynamic-cards`：完整界面回归，含窗口按钮、默认/最小窗口、长名称、16 种展开组合、保存分类、滚轮和滑块。
- 模拟截图文件名包含 `fixture`；设备和数值仅用于验证。`QWidget::grab()` 不包含窗口后方的桌面合成，不作为毛玻璃效果证明。
- 实际材质通过 computer-use 的 Windows.Graphics.Capture 查看，并使用独立彩色网格窗口验证背景透色和网格模糊；`state-output` 的 `ui.composition`、`ui.customChrome` 和 `ui.systemBackdropRequested` 仅记录请求及布局状态。

## 运行目录

实际启动入口为 `outputs/PeripheralHub-0.0/PeripheralHub.exe`。保留同目录 DLL、platforms、styles、qt.conf 和 data；不再使用旧的 `PeripheralHub-Glass` 路径。打包更新保留现有 data，也可用 `--data-dir` 指定独立测试配置。

## 统一界面与任务栏概览（2026-10-19）

主窗口和所有应用自有对话框共用 `UiTheme` 中的背景合成与窗口命中测试。设备详情、关联连接、删除档案、操作提示以及显示器写入确认均使用 `FrostedDialog`；取消按钮保持默认，标题栏关闭只拒绝对话框，不接受操作。窗口控制按钮不参与默认按钮或 Tab 焦点竞争。

显示器详情使用横向的标签、编辑器、操作按钮；窄空间中的下拉框省略文字，完整名称保留在弹出列表和提示中。已保存设备页、文本区、分隔条、横向滚动条及紧凑按钮共用主题；侧栏移除 C++ / Qt 实现说明。

运行中的主窗口（包括最小化状态）可向 Windows 提供四格任务栏缩略图：鼠标电量、键盘电量、显示器亮度、输出音量。缩略图内容不绘制“外设中心”标题或“查看完整界面”入口，离线和不可读值不替换成 0。悬停只读取当前界面缓存，不触发设备写入；状态变化以 250ms 合并通知系统更新。点击由 Windows 切回主窗口。

这是系统缩略图位图，不是在缩略图内嵌入可操作控件。Windows 控制浮层外框、系统标题和最大尺寸；收起到托盘后，主窗口不再是任务栏的窗口切换目标，不能承诺固定图标仍有窗口缩略图。内含浅色磨砂绘制，实际系统外层效果取决于 Windows。

官方接口：[DwmSetIconicThumbnail](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmseticonicthumbnail)、[WM_DWMSENDICONICTHUMBNAIL](https://learn.microsoft.com/en-us/windows/win32/dwm/wm-dwmsendiconicthumbnail)、[Taskbar Extensions](https://learn.microsoft.com/en-us/windows/win32/shell/taskbar-extensions)。位图使用 32 位预乘 alpha，系统复制后释放本地 HBITMAP；直播预览继续提供完整窗口图像。

`--taskbar-snapshot <path>` 可配合 `--quit-after` 导出当前四格内容，用于验证渲染；图像导出和 `ui.compactTaskbarPreviewRequested` 只证明本地渲染及系统请求结果，不等同于用户已完成实际悬停验收。

## 参考图布局（2026-10-19）

主窗口默认 1440×940，侧栏固定 230px 并贯穿背景；顶部“‹ / ›”按钮可把侧栏收起为 64px 图标栏，导航按钮保留图标、提示和可访问名称，收起状态写入 `settings.ini`。标题栏只占右侧内容区。设备图标 64px，标题、提示条、卡片间距与表单行按用户确认的参考图调整。窗口变小时不再切换另一套字号或图标，也不会重建整窗样式：总览页整体滚动，卡片继续保留自己的内部滚动，因此缩放或跨阈值时图标不会突然变大，界面也不会卡一下。滚动不改变设备选择、输入值或展开状态。

启动时以 1440×940 为首选尺寸，并按当前屏幕的可用工作区及原生窗口边框收缩、调整位置；设计下限为 960×600，并始终不超过当前工作区。运行中切换 DPI 或显示器、工作区尺寸变化时会重新计算下限并收紧窗口。`state-output` 的 `ui.window` 记录实际客户区、外框、可用工作区及 `fitsAvailableArea`，供部署自检使用。

显示器默认展开。常用页与展开页复用同一个亮度滑条，切页仅移动控件，不建立重复写入连接。只有单台显示器时，展开页隐藏重复的目标选择框；多台时仍显示选择入口。对比度使用单独的减号、加号按钮，编辑后仍须按“应用”；上下限来自实际设备，输入源和预设的原有能力校验、确认及目标变化保护保留。

详情、保存页、关联连接、操作提示及确认弹窗统一使用同一组材质和字体；设备详情增加清晰标题，诊断文本仍为只读。白色覆盖率降低以保留真实背景透色。背景色与模糊结果随窗口后方内容变化，无法在任意桌面上固定呈现参考图的蓝紫色。

设备名称、说明和诊断消息作为纯文本处理。选择框、卡片及保存页的提示文字对可被 Qt 识别为 HTML 的内容进行转义，避免名称中的标签或符号改变原文显示。

## 弹层与控件边缘（2026-10-19）

水平滑块的圆形手柄为 18×18 逻辑像素，轨道高 6px，控件最小高度 32px，避免 Windows 原生 15px 尺寸提示导致手柄上下裁切。亮度、输出音量和麦克风音量通过同一工厂创建，悬停、按下、键盘焦点与禁用状态共用主题。

选择框使用抗锯齿圆角绘制，保留蓝色键盘焦点提示。弹出列表的实际顶层窗口去除原生边框、系统阴影以及 Qt Windows 11 样式在初始化时追加的绘制阴影，透明圆角之外不再叠加矩形菜单框。选项文字仍支持完整显示；只有文字超出屏幕可用宽度时才保留横向浏览，不因闭合框本身过宽而额外出现滚动条。

应用菜单和完整名称提示共用透明顶层窗口与圆角边框。滚动区域的交界角设为透明；保存设备树去除 Windows 默认虚线连接线，使用 Fluent 展开箭头，保留原有展开、选择及键盘行为。设置页的选择框同样通过共享工厂创建。

`work/verification/popup-edges` 保存独立模拟控件检查、100%/125%/150%/200% 缩放结果及 Windows 平台渲染图。检查透明角、单层边框、短文字无额外滚动条、显示与隐藏不改变选择；完整界面回归另覆盖超长名称横向浏览与设置页控件。模拟程序不链接设备后端，不访问或写入真实硬件。
