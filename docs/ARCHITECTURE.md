# 架构与 C++ 阅读指南

## 模块与阅读顺序

界面负责呈现状态和绑定操作，服务负责后台任务与状态读回，Windows 后端负责系统接口，核心层保留可独立测试的 C++ 逻辑。后端实际支持什么，就向界面公布什么，未知能力不使用演示值。

| 阅读顺序 | 目录或文件 | 职责 |
|---|---|---|
| 1 | `src/core/live_value_control.h` | 不依赖 Qt 的实时值合并器；调用者提供时间，便于测试 |
| 2 | `src/app/DeviceService.*` | 工作线程、定时派发、取消旧目标请求和分类读回 |
| 3 | `src/app/MainWindow.*` | 界面呈现、目标选择、滑块与服务绑定 |
| 4 | `src/platform/WindowsBackend.h` | 稳定的纯 C++ 公开接口和数据结构 |
| 5 | `WindowsHid.cpp`、`WindowsAudio.cpp`、`WindowsMonitor.cpp` | 分别实现 HID、Core Audio、DDC/CI；资源封装留在各模块，共享错误信息和 GUID 工具在 `platform/internal/WindowsSupport.h` |
| 6 | `src/core/device_registry.*`、`vgn_protocol.*` | 设备身份、状态、协议编解码；由 `tests/core_tests.cpp` 验证 |

当前不增加服务端、云账号、数据库、动态插件系统或硬件屏幕程序。适配器先编译在程序中，以减少部署和 ABI 管理成本。

程序偏好由 `AppPreferences` / `AppSettings` 与 `widgets/SettingsPage` 分离管理：页面提交用户意图，配置模块只修改 INI 的程序选项，主窗口在保存成功后更新服务与档案跟踪器。设备参数仍留在原档案模块；说明见 `APP_SETTINGS.md`。

## 动态发现、统一调用与档案

`hid_discovery.*` 负责标准键鼠用途发现，不对未知 HID 发私有配置。`DeviceGrid` 只负责当前类别卡片显隐；`DeviceCard` 统一常用区与更多区的外观与交互。型号取系统信息，不在界面写死设备名称。

型号私有读取采用相同分层：`core/vgn_config.*` / `core/n9_protocol.*` 负责无 Qt 的数据结构、查询构造与校验；Windows 后端核对实际句柄并串行读取；`DeviceService` 管理工作线程和采样缓存；`MouseDetails` / `HeadsetDetails` 只负责显示；`ProfileTracker` 将成功读数交给 `DeviceProfiles` 原子保存。VGN 的档位 DPI、回报率和当前档位已通过单独的实验性写入入口接入，默认关闭；按键、其他性能字段及 N9 EQ 写入仍未作为产品功能开放。

`DeviceFeature` 是统一功能键，界面通过 `DeviceService::setFeature()` 提交参数，原有实时合并器、原生音频与 DDC 后端继续负责实际操作。输入和输出音量采用独立通道，避免互相取消；下拉框通过 `widgets/UiTheme.*` 中的 `createThemedComboBox()` 统一创建，样式与长名称适配也集中在该模块。存储由 `DeviceProfiles` 管理，`ProfileTracker` 协调连接会话、采样确认、立即保存和恢复；`SettingsRestore` 是独立纯 C++ 恢复逻辑，严格排除自动切源、复位与未知代码。行为、限制和调用示例见 `DEVICE_PROFILES.md`。

## 独立设备卡片

`src/app/widgets/DeviceCard.*` 封装常用视图与详情视图，外部通过 `quickLayout()` 和 `detailLayout()` 填充内容。卡片的尺寸建议不依赖当前视图，并关闭向父网格传递内容的随宽度变化高度；两页使用内部滚动区域，防止切换页面重新分配外层网格。小窗口中的内容保留正常最小高度，通过内部滚动访问。空白点击与标准“更多/收起”按钮切换同一个状态；滑块、输入、下拉、按钮和滚动条保留各自操作。自定义交互控件可设置 `deviceCardInteractive=true`，避免被当作空白区域。

`PercentDetails.*` 只处理百分比输入、预设及刷新请求，不访问设备。`setAvailable()`、`setValue()` 和 `setDescription()` 用于同步状态，`valueRequested(int)` 与 `refreshRequested()` 由主窗口绑定到原有服务。程序同步通过信号阻断防止反向写入；未知显示“—”并禁用写控件。输入框关闭键盘跟踪，确认完整数值后才提交；滑块的实时控制保持原有节流规则。主窗口在详情输入时保留编辑内容，在目标切换或失效时清空旧状态。

滚轮策略集中在 `UiTheme.*` 的 `createThemedComboBox()`、`createThemedSpinBox()` 和 `createThemedSlider()`：闭合选择框、数值框（含内部文本编辑器）和滑条将滚轮交给最近可滚动的外层视口；该层到边界再尝试外层，原事件消费一次。焦点不会解除这一保护。展开的下拉列表使用列表自身的滚动，浏览不提交选择；键盘与拖动仍沿用标准 Qt 行为。新增同类参数控件应复用这些工厂。

## 通用显示器内置功能

显示器标题从 DisplayConfig 友好名称更新，不绑定品牌。单屏自动选择稳定接口 ID，多屏由用户选择；显示通知合并刷新，轮询后备。`ddc_capabilities.*` 为独立纯 C++ 解析器；`MonitorControls.*` 仅展示后端检测到的对比度、输入源、情景和色彩预设，未验证名称保留原始代码。

`DeviceService::selectMonitorControlsTarget()` 先选择目标，`requestMonitorControls()` 异步读取，`setMonitorFeature()` 接受明确的功能代码和值。目标 generation 隔离迟到结果；后端每次重新打开唯一物理句柄并校验本项现值/范围，只允许四项白名单和声明中的枚举。服务可复用本连接成功读取的能力串，最长 5 分钟；强制刷新、目标变化、预设/输入源写入及失败使它失效，未命中则重新读能力串。缓存不落盘，不缓存句柄；标准原生 `setMonitorFeature()` 仍每次读取能力串。

设置只发一次，随后用 `queryMonitorFeature()` 确认目标原码；对比度立即安排回读，预设和输入源用 Qt 定时器等待 250 ms，不阻塞工作线程。首次未确认时，只再延后 250 ms 补读一次，不重发设置、不回滚。目标确认后再补读全部高级项，不把单项结果冒充完整能力快照。`monitorFeatureProgress` 与 `monitorFeatureFinished` 分别报告发送和确认状态，界面保留结果，后台补读不会覆盖。禁用应用按钮前清除其焦点，避免 Qt 将焦点转到后方输入并自动滚动卡片。常规扫描在阶段间避让用户高级写入，但不能打断已进入驱动的调用。不能通过枚举值最大值推算输入源或模式。详细实机证据和限制见 `MONITOR_CONTROLS.md`。

## 实时控制

`LiveValueControl` 只保存一个已派发请求和一个最新待写值。`DeviceService` 为音量设置 60 ms、亮度设置 200 ms 的最小派发间隔；请求完成后继续处理最新值，避免慢 DDC 操作积压。松手的 `finish` 会优先派发最终值，不必再等常规间隔，但不能抢占已经进入驱动的调用。

切换目标会清空未派发值并增加 generation；工作线程在请求开始前检查 generation，跳过已过期任务。已经开始的请求仍绑定原 ID，不能改投新设备。硬件读回与写入分开安排；拖动和等待读回时，界面保留用户目标值，旧快照不会把滑块拉回，匹配当前版本的读回完成后再呈现实际状态。

在现有 Qt 程序中复用服务时，顺序为：

```cpp
service.selectVolumeTarget(endpointId);      // 目标来自枚举结果
service.setFeature(endpointId, DeviceFeature::Volume, percent); // valueChanged 时提交
service.finishVolume(endpointId);           // sliderReleased 时优先提交该端点最终值
```

亮度对应 `selectBrightnessTarget`、`setFeature(id, DeviceFeature::Brightness, percent)`、`finishBrightness`。旧的 `setVolume`、`setBrightness` 仍为内部专用入口，界面统一通过 `setFeature` 提交。服务的这些方法由所属 Qt 线程调用，实际设备操作由其工作线程串行执行；通过 `refreshed` 和 `snapshot()` 取得当前读回状态，调节失败由 `operationFinished` 报告。直接使用 `LiveValueControl` 时也必须先 `selectTarget` 再 `request`，每次 `take` 后按 token 调用 `complete`；它自身不创建线程或定时器。

## 最小原生调用入口

不需要 Qt 界面时可链接 `peripheral_windows`，包含 `platform/WindowsBackend.h`。以下片段在调用者的工作线程中串行执行；`selectedId` 必须是先前枚举并明确选择的唯一播放端点 ID，不能凭名称或列表第一项猜测：

只构建原生库可执行 `scripts/build.ps1 -NoGui -BuildDirectory "$PWD\work\native" -RunTests`，该模式不查找 Qt。其他 CMake 工程也可使用 `-DPERIPHERAL_BUILD_APP=OFF`，链接 `peripheral_windows` 或 `peripheral_core`。原生模块只依赖 C++ 标准库与 Windows SDK。

```cpp
try {
    peripheral::platform::WindowsBackend api;
    const auto endpoints = api.enumerateAudioEndpoints();
    const auto count = std::count_if(endpoints.begin(), endpoints.end(),
        [&](const auto& e) { return e.id == selectedId && !e.capture && e.volumeReadable; });
    if (count != 1) throw std::runtime_error("Selected endpoint is not uniquely available");
    api.setAudioVolume(selectedId, desiredPercent); // 0..100，失败抛异常
    const auto readback = api.enumerateAudioEndpoints(); // 按同一 ID 取读回结果
} catch (const std::exception& error) {
    // 记录 error.what()，并通过调用方的消息机制通知界面。
}
```

片段额外需要 `<algorithm>`、`<stdexcept>`。显示器对应 `enumerateMonitors` 与 `setMonitorBrightness`，先确认唯一 ID 和 `brightnessReadable`，写后重新读取。一次原生调用不含滑块合并逻辑；连续交互请复用服务或核心合并器。当前为源码/CMake 库复用接口，没有承诺 DLL 插件 ABI。

## 需要分开的概念

| 概念 | 含义 |
|---|---|
| 物理设备档案 | 用户认识的那台鼠标、键盘或耳机 |
| 接口 | 接收器 HID、USB 有线 HID、音频端点等系统接口 |
| 连接状态 | 接口存在、设备响应、暂时无响应、明确断开 |
| 能力 | 当前接口可否读取电量、写 DPI、设亮度等 |
| 读数 | 数值、来源、最后成功时间、是否缓存 |

不能因为两种接口的品牌相同，就把它们无条件归为同一物理设备。首版仅在可确认的型号范围做匹配；之后可根据序列号、容器 ID、协议身份及用户确认建立稳定关联。

## 多连接模式

充电状态独立于通信通道。设备可能一边使用接收器通信、一边通过 USB 充电，也可能插线后改走有线。实现时需要保留每个接口的能力和最近响应时间，再选择可用的查询路径。

通道切换应重新枚举并重新验证身份。控制写入只发往一个已确认目标；读数失效后应明确显示未知或缓存，而不是长期显示旧值。

## Windows 资源与线程

- HID 和 Win32 句柄需要在离开作用域时关闭；使用 RAII 封装，避免异常或早返回造成泄漏。
- COM 接口需要与初始化线程和引用计数配套，建议通过智能指针管理。
- 物理显示器句柄要对应释放；不要把一次枚举得到的句柄跨越显示器拔插后继续使用。
- HID 和 DDC 读写不能长时间占用界面线程；后台工作完成后通过 Qt 信号传递值。
- 同一接口的私有协议请求应串行；滑块写入应合并，避免堆积大量过期目标值。

初版的具体超时、去重和恢复能力以代码及硬件测试为准；这里是实现约束，不能代替测试结果。

## 未来扩展

先增加已验证型号的能力，再抽取共用传输层，避免在协议尚未知时先设计复杂插件框架。后续独立屏幕可消费带版本号的 JSON 状态快照；它只需要设备标识、能力、读数、来源和时间戳。当前不实现串口、Wi-Fi 或硬件固件。

## 官方接口资料

- [Windows HID](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/introduction-to-hid-concepts)
- [Core Audio EndpointVolume](https://learn.microsoft.com/en-us/windows/win32/coreaudio/endpointvolume-api)
- [Windows Monitor Configuration](https://learn.microsoft.com/en-us/windows/win32/monitor/monitor-configuration)
- [Qt Widgets](https://doc.qt.io/qt-6/qtwidgets-index.html)
